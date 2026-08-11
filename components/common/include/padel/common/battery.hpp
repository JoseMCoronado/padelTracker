// Court-unit Li-ion helpers: voltage → SoC and rough runtime estimate.
// Hardware measurement lives in the board profile; this is pure math.
#pragma once

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

}  // namespace padel::battery
