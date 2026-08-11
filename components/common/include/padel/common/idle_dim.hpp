// Idle backlight policy for the court display: pure decisions only, the
// board profile owns the PWM writes.
#pragma once

#include <cstdint>

namespace padel::power {

enum class DisplayStage : std::uint8_t { Awake, Dimmed, Off };

struct IdlePolicy {
    std::uint32_t dim_after_ms = 10u * 60u * 1000u;
    std::uint32_t off_after_ms = 30u * 60u * 1000u;
    // Backlight level held while dimmed. 0 disables the whole policy so the
    // panel never leaves Awake.
    std::uint8_t dim_percent = 15;
};

// Stage for a given idle duration. dim_percent == 0 or dim_after_ms == 0
// pins the panel to Awake; off_after_ms == 0 keeps the backlight on no matter
// how long the idle window grows.
DisplayStage stage_for_idle(std::uint32_t idle_ms, const IdlePolicy& policy);

// Backlight percent to write: the organizer level while Awake, the dim level
// (never brighter than the organizer level) while Dimmed, 0 while Off.
std::uint8_t applied_percent(DisplayStage stage, std::uint8_t user_percent,
                             const IdlePolicy& policy);

// Diagnostics label: "awake" / "dimmed" / "backlight off".
const char* stage_label(DisplayStage stage);

}  // namespace padel::power
