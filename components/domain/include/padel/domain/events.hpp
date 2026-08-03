#pragma once

#include <optional>
#include <variant>

#include "padel/common/ids.hpp"
#include "padel/domain/types.hpp"

namespace padel::domain {

// Events are accepted facts. Only underivable facts are journaled; game/set/
// match completion are computed by the reducer from PointAwarded (ADR-0003).

struct MatchCreated {
    MatchId match_id{};
    MatchConfig config{};
};

struct MatchStarted {
    TeamId initial_serving_team{TeamId::A};
};

struct PointAwarded {
    TeamId team{};
    InputSource source{InputSource::Simulator};
};

// Compensating event (ADR-0004). The referenced PointAwarded is skipped
// during replay; it is never deleted or mutated.
struct ScoringActionUndone {
    EventId undone_event_id{};
};

struct ServingTeamChanged {
    TeamId team{};
};

struct MatchPaused {};

struct MatchResumed {};

struct MatchFinishedManually {
    std::optional<TeamId> declared_winner{};
};

// Archives nothing by itself; the journal preceding it is the archive.
struct MatchReset {};

using Event = std::variant<MatchCreated,
                           MatchStarted,
                           PointAwarded,
                           ScoringActionUndone,
                           ServingTeamChanged,
                           MatchPaused,
                           MatchResumed,
                           MatchFinishedManually,
                           MatchReset>;

struct StoredEvent {
    EventId id{};
    Event payload{};
};

}  // namespace padel::domain
