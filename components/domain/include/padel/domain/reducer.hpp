#pragma once

#include "padel/domain/events.hpp"
#include "padel/domain/types.hpp"

namespace padel::domain {

// Pure state transition: no I/O, no framework dependencies, natively testable.
// ScoringActionUndone is a no-op here; compensation happens at replay time by
// skipping the referenced event (see MatchEngine::rebuild, ADR-0004).
MatchState apply(MatchState state, const Event& event);

// True if the deciding set (both teams one set from victory) is played as a
// match tiebreak under this config.
bool deciding_set_is_match_tiebreak(const MatchState& state);

}  // namespace padel::domain
