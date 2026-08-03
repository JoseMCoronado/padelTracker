#pragma once

#include <optional>
#include <vector>

#include "padel/common/ids.hpp"
#include "padel/common/result.hpp"
#include "padel/domain/commands.hpp"
#include "padel/domain/events.hpp"
#include "padel/domain/reducer.hpp"
#include "padel/domain/types.hpp"

namespace padel::domain {

// Owns the event journal and authoritative state for one match. Commands are
// validated against current state; accepted commands append exactly one event.
// In firmware, a single application task owns this object (spec section 23.3)
// and the journal is mirrored to durable storage (M3).
class MatchEngine {
public:
    explicit MatchEngine(MatchConfig config, MatchId match_id = 1);

    using CommandResult = Result<EventId, CommandError>;

    CommandResult handle(const StartMatch& cmd);
    CommandResult handle(const AwardPoint& cmd);
    CommandResult handle(const UndoLastScoringAction& cmd);
    CommandResult handle(const SetServingTeam& cmd);
    CommandResult handle(const PauseMatch& cmd);
    CommandResult handle(const ResumeMatch& cmd);
    CommandResult handle(const FinishMatchManually& cmd);
    CommandResult handle(const ResetMatch& cmd);

    const MatchState& state() const { return state_; }
    const std::vector<StoredEvent>& journal() const { return events_; }

    // What the next undo would compensate, if anything (for the organizer
    // confirmation preview, e.g. "Undo Team A point?").
    std::optional<PointAwarded> next_undo_target() const;

    // Rebuild state from an externally recovered journal (boot recovery, M3).
    static MatchEngine replay(std::vector<StoredEvent> events, MatchConfig fallback_config);

private:
    MatchEngine() = default;

    EventId append(Event event);
    void rebuild();
    std::optional<EventId> find_undo_target() const;

    std::vector<StoredEvent> events_{};
    MatchState state_{};
    EventId next_event_id_ = 1;
};

}  // namespace padel::domain
