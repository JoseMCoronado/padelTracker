#include "padel/common/battery.hpp"

#include <cinttypes>
#include <cstdio>

namespace padel::battery {
namespace {

struct Anchor {
    std::uint16_t mv;
    std::uint8_t percent;
};

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

}  // namespace padel::battery
