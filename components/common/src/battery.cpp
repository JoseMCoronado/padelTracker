#include "padel/common/battery.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>

namespace padel::battery {
namespace {

struct Anchor {
    std::uint16_t mv;
    std::uint8_t percent;
};

std::uint16_t abs_diff(std::uint16_t a, std::uint16_t b) {
    return static_cast<std::uint16_t>(a > b ? a - b : b - a);
}

// Typical single-cell Li-ion OCV curve (approximate).
constexpr Anchor kAnchors[] = {
    {4200, 100}, {3900, 75}, {3700, 50}, {3500, 25}, {3300, 10}, {3000, 0},
};

}  // namespace

std::optional<std::uint8_t> mv_to_percent(std::uint16_t mv) {
    if (mv < kNoCellMv) {
        return std::nullopt;
    }
    if (mv >= kAnchors[0].mv) {
        return 100;
    }
    const auto last = kAnchors[sizeof(kAnchors) / sizeof(kAnchors[0]) - 1];
    if (mv <= last.mv) {
        return 0;
    }
    for (std::size_t i = 0; i + 1 < sizeof(kAnchors) / sizeof(kAnchors[0]); ++i) {
        const Anchor hi = kAnchors[i];
        const Anchor lo = kAnchors[i + 1];
        if (mv <= hi.mv && mv >= lo.mv) {
            const std::uint16_t span_mv = static_cast<std::uint16_t>(hi.mv - lo.mv);
            const std::uint8_t span_pct = static_cast<std::uint8_t>(hi.percent - lo.percent);
            const std::uint16_t above_lo = static_cast<std::uint16_t>(mv - lo.mv);
            return static_cast<std::uint8_t>(lo.percent +
                                            (above_lo * span_pct) / span_mv);
        }
    }
    return 0;
}

std::uint32_t estimate_runtime_min(std::uint8_t soc_percent, std::uint16_t capacity_mah,
                                   std::uint16_t avg_draw_ma) {
    if (soc_percent == 0 || capacity_mah == 0 || avg_draw_ma == 0) {
        return 0;
    }
    // minutes = capacity_mAh * soc/100 / draw_mA * 60
    return (static_cast<std::uint32_t>(capacity_mah) * soc_percent * 60u) /
           (static_cast<std::uint32_t>(avg_draw_ma) * 100u);
}

std::string format_runtime_estimate(std::optional<std::uint8_t> soc_percent,
                                    std::uint16_t capacity_mah, std::uint16_t avg_draw_ma) {
    if (!soc_percent) {
        return "unknown";
    }
    const std::uint32_t minutes =
        estimate_runtime_min(*soc_percent, capacity_mah, avg_draw_ma);
    if (minutes == 0) {
        return "~0m";
    }
    const std::uint32_t hours = minutes / 60u;
    const std::uint32_t rem = minutes % 60u;
    char buf[24];
    if (hours > 0) {
        std::snprintf(buf, sizeof(buf), "~%" PRIu32 "h %" PRIu32 "m", hours, rem);
    } else {
        std::snprintf(buf, sizeof(buf), "~%" PRIu32 "m", rem);
    }
    return buf;
}

std::optional<std::uint16_t> median_mv(const std::uint16_t* samples, std::size_t count) {
    if (samples == nullptr || count == 0) {
        return std::nullopt;
    }
    // A burst is a handful of reads; a fixed buffer keeps this allocation-free
    // on the court unit. Samples past the buffer are ignored.
    std::array<std::uint16_t, 32> sorted{};
    const std::size_t n = std::min(count, sorted.size());
    std::copy_n(samples, n, sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n));
    if (n % 2 == 1) {
        return sorted[n / 2];
    }
    const std::uint32_t lo = sorted[n / 2 - 1];
    const std::uint32_t hi = sorted[n / 2];
    return static_cast<std::uint16_t>((lo + hi + 1) / 2);
}

void BatteryMonitor::reset() {
    smoothed_mv_.reset();
    percent_.reset();
    low_ = false;
    misses_ = 0;
}

void BatteryMonitor::add_sample(std::optional<std::uint16_t> mv) {
    const bool plausible =
        mv.has_value() && *mv >= kNoCellMv &&
        (!smoothed_mv_ || abs_diff(*mv, *smoothed_mv_) <= config_.outlier_mv);
    if (!plausible) {
        if (!smoothed_mv_) {
            return;  // already unknown; nothing to hold on to
        }
        if (misses_ < 255) {
            ++misses_;
        }
        if (misses_ >= config_.misses_to_unknown) {
            reset();
        }
        return;
    }
    misses_ = 0;

    if (!smoothed_mv_) {
        smoothed_mv_ = *mv;  // seed on the first good read: no ramp from zero
    } else {
        const int divisor = config_.ema_divisor == 0 ? 1 : config_.ema_divisor;
        const int delta = static_cast<int>(*mv) - static_cast<int>(*smoothed_mv_);
        // Round away from zero, otherwise a small steady drift truncates to
        // no movement at all and the estimate never catches up.
        const int bias = delta >= 0 ? divisor - 1 : -(divisor - 1);
        const int step = (delta + bias) / divisor;
        smoothed_mv_ = static_cast<std::uint16_t>(static_cast<int>(*smoothed_mv_) + step);
    }

    const auto target = mv_to_percent(*smoothed_mv_);
    if (!target) {
        reset();
        return;
    }
    const std::uint8_t max_step = config_.max_pct_step == 0 ? 1 : config_.max_pct_step;
    if (!percent_) {
        percent_ = *target;
    } else if (*target > *percent_) {
        const std::uint8_t room = static_cast<std::uint8_t>(*target - *percent_);
        percent_ = static_cast<std::uint8_t>(*percent_ + std::min(room, max_step));
    } else if (*target < *percent_) {
        const std::uint8_t room = static_cast<std::uint8_t>(*percent_ - *target);
        percent_ = static_cast<std::uint8_t>(*percent_ - std::min(room, max_step));
    }

    if (*percent_ <= config_.low_enter_pct) {
        low_ = true;
    } else if (*percent_ >= config_.low_exit_pct) {
        low_ = false;
    }
}

}  // namespace padel::battery
