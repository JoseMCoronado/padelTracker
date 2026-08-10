#pragma once

#include <optional>

#include "padel/common/ids.hpp"
#include "padel/domain/types.hpp"

namespace padel::domain {

// Commands express requested actions and may be rejected (spec section 8.5).
// The engine validates a command against current state and, if accepted,
// appends the corresponding event.

struct CreateMatch {
    MatchId match_id{};
    MatchConfig config{};
};

struct StartMatch {
    TeamId initial_serving_team{TeamId::A};
};

struct AwardPoint {
    TeamId team{};
    InputSource source{InputSource::Simulator};
};

struct UndoLastScoringAction {
    InputSource source{InputSource::TouchscreenAdmin};
    // When set, the undo only proceeds if the most recent point belongs to
    // this team, so a remote can take back its own team's point but never
    // the opponents' (ADR-0014). The organizer's undo leaves this empty.
    std::optional<TeamId> only_team{};
};

struct SetServingTeam {
    TeamId team{};
};

struct PauseMatch {};
struct ResumeMatch {};

struct FinishMatchManually {
    std::optional<TeamId> declared_winner{};
};

// Requires a protected organizer flow upstream (spec section 8.5); the engine
// additionally never maps this to a remote point press.
struct ResetMatch {};

enum class CommandError : std::uint8_t {
    MatchNotActive,
    MatchAlreadyStarted,
    MatchNotStarted,
    MatchCompleted,
    MatchPausedError,
    NothingToUndo,
};

}  // namespace padel::domain
