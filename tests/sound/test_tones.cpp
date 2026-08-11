// Buzzer cue table (spec section 15, ADR-0018). The point of these tests is
// that the sounds stay tellable apart by ear, which is the whole reason the
// court unit moved from durations to pitches.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "padel/sound/tones.hpp"

using namespace padel;

namespace {

const std::vector<sound::Cue> kAllCues = {
    sound::Cue::PointScored,  sound::Cue::GameComplete,     sound::Cue::SetComplete,
    sound::Cue::RemoteUndo,   sound::Cue::PairingConfirmed, sound::Cue::MatchComplete,
    sound::Cue::SelfTest,
};

// The audible shape of a cue: rests carry no pitch, so they are not what makes
// two cues different to a listener.
std::vector<std::uint16_t> pitches(sound::Cue cue) {
    const sound::Pattern pattern = sound::pattern_for(cue);
    std::vector<std::uint16_t> result;
    for (std::size_t i = 0; i < pattern.count; ++i) {
        if (pattern.tones[i].freq_hz != 0) {
            result.push_back(pattern.tones[i].freq_hz);
        }
    }
    return result;
}

}  // namespace

TEST_CASE("every cue has a named, playable pattern") {
    for (const sound::Cue cue : kAllCues) {
        const sound::Pattern pattern = sound::pattern_for(cue);
        REQUIRE(pattern.count > 0);
        CHECK(pattern.tones != nullptr);
        CHECK(std::string(pattern.name).empty() == false);
        for (std::size_t i = 0; i < pattern.count; ++i) {
            // A zero-length step would be inaudible and would still cost a
            // timer round trip.
            CHECK(pattern.tones[i].duration_ms > 0);
        }
    }
}

TEST_CASE("tones stay inside the band a piezo can actually carry") {
    for (const sound::Cue cue : kAllCues) {
        for (const std::uint16_t hz : pitches(cue)) {
            CHECK(hz >= sound::kMinToneHz);
            CHECK(hz <= sound::kMaxToneHz);
        }
    }
}

TEST_CASE("no two cues share the same sequence of pitches") {
    for (std::size_t i = 0; i < kAllCues.size(); ++i) {
        for (std::size_t j = i + 1; j < kAllCues.size(); ++j) {
            CHECK(pitches(kAllCues[i]) != pitches(kAllCues[j]));
        }
    }
}

TEST_CASE("a point rises and an undo falls") {
    const auto point = pitches(sound::Cue::PointScored);
    REQUIRE(point.size() >= 2);
    for (std::size_t i = 1; i < point.size(); ++i) {
        CHECK(point[i] > point[i - 1]);
    }

    const auto undo = pitches(sound::Cue::RemoteUndo);
    REQUIRE(undo.size() >= 2);
    for (std::size_t i = 1; i < undo.size(); ++i) {
        CHECK(undo[i] < undo[i - 1]);
    }

    // Both cues sit in the loud top of the band so they carry across a court,
    // which means pitch alone no longer separates them. What does: opposite
    // contours, more steps, and a run several times longer than a point.
    CHECK(undo.size() > point.size());
    CHECK(sound::duration_ms(sound::pattern_for(sound::Cue::RemoteUndo)) >
          3 * sound::duration_ms(sound::pattern_for(sound::Cue::PointScored)));
}

TEST_CASE("the point cue is short enough for back-to-back points") {
    const std::uint32_t point_ms = sound::duration_ms(sound::pattern_for(sound::Cue::PointScored));
    CHECK(point_ms > 0);
    // Two rallies never finish within 200 ms of each other, so no point can
    // swallow its predecessor's cue.
    CHECK(point_ms <= 200);
    CHECK(point_ms < sound::duration_ms(sound::pattern_for(sound::Cue::RemoteUndo)));
}

TEST_CASE("point, game, set and match cues escalate in length") {
    const auto ms = [](sound::Cue cue) { return sound::duration_ms(sound::pattern_for(cue)); };
    // Only one of these plays per point, so their lengths are what tells a
    // listener how far the match just moved.
    CHECK(ms(sound::Cue::PointScored) < ms(sound::Cue::GameComplete));
    CHECK(ms(sound::Cue::GameComplete) < ms(sound::Cue::SetComplete));
    CHECK(ms(sound::Cue::SetComplete) < ms(sound::Cue::MatchComplete));
}

TEST_CASE("duration_ms sums every step, rests included") {
    const sound::Pattern undo = sound::pattern_for(sound::Cue::RemoteUndo);
    std::uint32_t expected = 0;
    for (std::size_t i = 0; i < undo.count; ++i) {
        expected += undo.tones[i].duration_ms;
    }
    CHECK(sound::duration_ms(undo) == expected);
    CHECK(sound::duration_ms(sound::Pattern{}) == 0);
}

TEST_CASE("the self test sweeps the whole band to find the element's resonance") {
    const auto sweep = pitches(sound::Cue::SelfTest);
    REQUIRE(sweep.size() >= 5);
    CHECK(sweep.front() == sound::kMinToneHz);
    CHECK(sweep.back() == sound::kMaxToneHz);
    for (std::size_t i = 1; i < sweep.size(); ++i) {
        CHECK(sweep[i] > sweep[i - 1]);
    }
}
