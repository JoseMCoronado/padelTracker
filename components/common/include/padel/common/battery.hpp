// Court-unit Li-ion helpers: voltage → SoC and rough runtime estimate.
// Hardware measurement lives in the board profile; this is pure math.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace padel::battery {

// CITYORK 3.7 V 2000 mAh 103450 on the Waveshare 7B PH2.0 header.
inline constexpr std::uint16_t kCourtBatteryCapacityMah = 2000;

// Assumed average cell draw while the court panel is active. Derived from
// Waveshare's 5 V / 350 mA board figure through a ~90% boost: ~475 mA.
// Revisit after a real discharge measurement.
inline constexpr std::uint16_t kCourtAvgDrawMa = 475;

// ADC / open-circuit readings below this are treated as "no cell".
inline constexpr std::uint16_t kNoCellMv = 2500;

// Piecewise Li-ion open-circuit voltage → SoC percent. Empty when mv is
// below kNoCellMv (USB-only / disconnected). Values are clamped to 0–100.
std::optional<std::uint8_t> mv_to_percent(std::uint16_t mv);

// Estimated remaining runtime in minutes from SoC and capacity/draw.
// Returns 0 when soc is 0 or draw is 0.
std::uint32_t estimate_runtime_min(std::uint8_t soc_percent,
                                   std::uint16_t capacity_mah = kCourtBatteryCapacityMah,
                                   std::uint16_t avg_draw_ma = kCourtAvgDrawMa);

// Human label for diagnostics, e.g. "~2h 15m" / "~45m" / "unknown".
std::string format_runtime_estimate(std::optional<std::uint8_t> soc_percent,
                                    std::uint16_t capacity_mah = kCourtBatteryCapacityMah,
                                    std::uint16_t avg_draw_ma = kCourtAvgDrawMa);

// Median of a burst of readings. Empty when count is 0. The caller passes
// only successful reads; a median (not a mean) is used so one corrupt I2C
// transfer in the burst cannot drag the result.
std::optional<std::uint16_t> median_mv(const std::uint16_t* samples, std::size_t count);

// Turns the noisy, under-load ADC feed into a percentage stable enough to
// sit on a scoreboard. The raw court reading swings with the backlight
// boost, panel refresh and radio bursts, and one ADC count is already ~1.2
// SoC points in the middle of the OCV curve, so an unfiltered sample jumps
// by tens of points between reads.
//
// Clock-free: the caller owns the sample cadence, and the defaults assume
// the court's 5 s tick (~40 s smoothing constant, unknown after ~30 s of
// failed reads).
class BatteryMonitor {
public:
    struct Config {
        // A sample this far from the running estimate is treated as a bad
        // read rather than a real step: cells do not move that fast.
        std::uint16_t outlier_mv = 300;
        // Weight of each accepted sample in the millivolt average: 1/n.
        std::uint8_t ema_divisor = 8;
        // Largest displayed percent change per accepted sample.
        std::uint8_t max_pct_step = 1;
        // Consecutive bad samples held before the value goes unknown.
        std::uint8_t misses_to_unknown = 6;
        // Warning latches at or below enter, clears at or above exit.
        std::uint8_t low_enter_pct = 15;
        std::uint8_t low_exit_pct = 20;
    };

    BatteryMonitor() = default;
    explicit BatteryMonitor(Config config) : config_(config) {}

    // Feeds one reading; empty means the read failed. Readings below
    // kNoCellMv and implausible jumps are held, not shown.
    void add_sample(std::optional<std::uint16_t> mv);

    // Filtered millivolts, for diagnostics. Empty until the first good read.
    std::optional<std::uint16_t> smoothed_mv() const { return smoothed_mv_; }

    // Slew-limited SoC for the UI. Empty when unknown / no cell.
    std::optional<std::uint8_t> percent() const { return percent_; }

    // Low-battery warning with hysteresis, so it cannot flicker on the
    // threshold. False while the level is unknown.
    bool low() const { return low_; }

    // Consecutive bad samples since the last good one, for diagnostics.
    std::uint8_t misses() const { return misses_; }

    // Drops to unknown, as if no cell had ever been read.
    void reset();

private:
    Config config_{};
    std::optional<std::uint16_t> smoothed_mv_{};
    std::optional<std::uint8_t> percent_{};
    bool low_ = false;
    std::uint8_t misses_ = 0;
};

}  // namespace padel::battery
