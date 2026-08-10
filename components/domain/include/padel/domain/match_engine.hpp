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

// A validated command outcome that has not been applied yet. Carries the
// identity the event will get on commit, so a durable journal record can be
// written *before* the state changes (spec section 13.3: Accepted is only
// ACKed after the event is durable). Commit must follow decide with no other
// command in between.
struct DecidedEvent {
    EventId id{};
    std::uint64_t revision_after{};
    Event payload{};
};

// Owns the event journal and authoritative state for one match. Commands are
// validated against current state; accepted commands append exactly one event.
// In firmware, a single application task owns this object (spec section 23.3)
// and the journal is mirrored to durable storage (M3).
//
// Two usage styles:
//   - handle(cmd): validate + apply in one step (CLI simulator, tests).
//   - decide(cmd) -> persist durably -> commit(decided): two-phase path used
//     by the application service so a storage failure leaves state untouched.
class MatchEngine {
public:
    explicit MatchEngine(MatchConfig config, MatchId match_id = 1);

    using CommandResult = Result<EventId, CommandError>;
    using DecideResult = Result<DecidedEvent, CommandError>;

    DecideResult decide(const StartMatch& cmd) const;
    DecideResult decide(const AwardPoint& cmd) const;
    DecideResult decide(const UndoLastScoringAction& cmd) const;
    DecideResult decide(const SetServingTeam& cmd) const;
    DecideResult decide(const PauseMatch& cmd) const;
    DecideResult decide(const ResumeMatch& cmd) const;
    DecideResult decide(const FinishMatchManually& cmd) const;
    DecideResult decide(const ResetMatch& cmd) const;

    // Applies a decided event. Must be the most recent decide() outcome; the
    // engine asserts the event id is still the next id.
    EventId commit(const DecidedEvent& decided);

    template <typename Command>
    CommandResult handle(const Command& cmd) {
        const DecideResult decided = decide(cmd);
        if (!decided) {
            return CommandResult::err(decided.error());
        }
        return CommandResult::ok(commit(decided.value()));
    }

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
    std::optional<EventId> find_undo_target(std::optional<TeamId> only_team) const;
    DecidedEvent make_decided(Event payload) const;

    std::vector<StoredEvent> events_{};
    MatchState state_{};
    EventId next_event_id_ = 1;
};

}  // namespace padel::domain
