#pragma once

#include "padel/sound/tones.hpp"

// The court unit's only sound output (spec section 15). Patterns come from
// components/sound; this turns them into pin wiggling and nothing else.
namespace buzzer {

// Configures the output and the step timer. Call once, before any play().
void init();

// Starts a cue and returns immediately; the pattern plays on the esp_timer
// task. A second call replaces whatever is sounding, so a late-arriving cue
// is never queued behind a stale one.
void play(padel::sound::Cue cue);

// Which kind of sounder the build drives, for the diagnostics screen.
const char* kind();

}  // namespace buzzer
