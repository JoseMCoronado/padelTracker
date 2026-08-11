#pragma once

#include <cstddef>
#include <cstdint>

// Everything the court unit says out loud, as data (spec section 15).
// Portable on purpose: the firmware turns a pattern into LEDC frequencies,
// court-sim prints it, and native tests assert on it, so the sounds are
// defined in exactly one place.
namespace padel::sound {

// One step of a pattern. A zero frequency is a deliberate silence, which is
// the only way two notes of the same pitch stay separate.
struct Tone {
    std::uint16_t freq_hz;
    std::uint16_t duration_ms;
};

// Cues are told apart by pitch shape, not by length: from the far side of a
// court, 80 ms and 150 ms of the same note are the same sound, while a rise
// and a fall are not (ADR-0018).
enum class Cue : std::uint8_t {
    PointScored,
    GameComplete,
    SetComplete,
    RemoteUndo,
    PairingConfirmed,
    MatchComplete,
    SelfTest,
};

struct Pattern {
    const Tone* tones = nullptr;
    std::size_t count = 0;
    const char* name = "";
};

Pattern pattern_for(Cue cue);

// Total time the pattern occupies, silences included.
std::uint32_t duration_ms(const Pattern& pattern);

// A piezo element is only loud near its mechanical resonance and falls off
// steeply either side, so every tone stays inside the band where the sounders
// we use still carry across a hall. Enforced by a native test.
inline constexpr std::uint16_t kMinToneHz = 1000;
inline constexpr std::uint16_t kMaxToneHz = 5000;

}  // namespace padel::sound
