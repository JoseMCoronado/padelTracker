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
