#include "padel/sound/tones.hpp"

#include <iterator>

namespace padel::sound {
namespace {

// Named by equal-temperament pitch, all inside the 1-5 kHz band. The exact
// note matters far less than the direction: rising = something was won,
// falling = something was taken away.
constexpr std::uint16_t kC7 = 2093;
constexpr std::uint16_t kE7 = 2637;
constexpr std::uint16_t kG7 = 3136;
constexpr std::uint16_t kA7 = 3520;
constexpr std::uint16_t kC8 = 4186;

// The two cues the remotes trigger live at the top of the band, because that
// is where the element we fitted is loudest and they are the two that have to
// carry across a court. Anything below ~2.5 kHz is audibly weaker on it.
constexpr Tone kPointScored[] = {{kG7, 45}, {kC8, 80}};

// An undo fires with nobody near the court unit, so it must not be mistaken
// for a score. It keeps the loud notes and separates itself by shape instead:
// a three-step descending run, five times longer than a point.
constexpr Tone kRemoteUndo[] = {{kC8, 150}, {kA7, 150}, {kE7, 300}};

constexpr Tone kPairingConfirmed[] = {{kC7, 60}, {kE7, 60}, {kG7, 90}};

constexpr Tone kMatchComplete[] = {{kC7, 110}, {kE7, 110}, {kG7, 110}, {0, 60}, {kC8, 320}};

// Diagnostics plays a stepped sweep rather than one note: it proves the wiring
// and it locates the element's resonance by ear, since a piezo can be far
// louder at one step than at its neighbours. Whichever step rings loudest is
// where the cues above should sit.
constexpr Tone kSelfTest[] = {{1000, 120}, {1500, 120}, {2000, 120}, {2500, 120}, {3000, 120},
                              {3500, 120}, {4000, 120}, {4500, 120}, {5000, 120}};

}  // namespace

Pattern pattern_for(Cue cue) {
    switch (cue) {
        case Cue::PointScored:
            return {kPointScored, std::size(kPointScored), "point"};
        case Cue::RemoteUndo:
            return {kRemoteUndo, std::size(kRemoteUndo), "undo"};
        case Cue::PairingConfirmed:
            return {kPairingConfirmed, std::size(kPairingConfirmed), "paired"};
        case Cue::MatchComplete:
            return {kMatchComplete, std::size(kMatchComplete), "match complete"};
        case Cue::SelfTest:
            return {kSelfTest, std::size(kSelfTest), "self test sweep"};
    }
    return {};
}

std::uint32_t duration_ms(const Pattern& pattern) {
    std::uint32_t total = 0;
    for (std::size_t i = 0; i < pattern.count; ++i) {
        total += pattern.tones[i].duration_ms;
    }
    return total;
}

}  // namespace padel::sound
