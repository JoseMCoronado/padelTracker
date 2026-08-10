#include "padel/domain/match_engine.hpp"

#include <algorithm>
#include <cassert>
#include <unordered_set>

namespace padel::domain {

MatchEngine::MatchEngine(MatchConfig config, MatchId match_id) {
    append(MatchCreated{match_id, config});
}

EventId MatchEngine::append(Event event) {
    const EventId id = next_event_id_++;
    events_.push_back(StoredEvent{id, std::move(event)});
    state_ = padel::domain::apply(std::move(state_), events_.back().payload);
    // Revision counts appended events, monotonic even across undo rebuilds.
    state_.revision = events_.size();
    return id;
}

void MatchEngine::rebuild() {
    std::unordered_set<EventId> compensated;
    for (const StoredEvent& stored : events_) {
        if (const auto* undo = std::get_if<ScoringActionUndone>(&stored.payload)) {
            compensated.insert(undo->undone_event_id);
        }
    }

    MatchState state{};
    for (const StoredEvent& stored : events_) {
        if (std::holds_alternative<PointAwarded>(stored.payload) &&
            compensated.count(stored.id) != 0) {
            continue;
        }
        state = padel::domain::apply(std::move(state), stored.payload);
    }
    state.revision = events_.size();
    state_ = std::move(state);
}

std::optional<EventId> MatchEngine::find_undo_target(std::optional<TeamId> only_team) const {
    std::unordered_set<EventId> compensated;
    // Walk backward; stop at match boundaries so undo never resurrects
    // points from before a reset.
    for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
        if (std::holds_alternative<MatchReset>(it->payload) ||
            std::holds_alternative<MatchCreated>(it->payload)) {
            return std::nullopt;
        }
        if (const auto* undo = std::get_if<ScoringActionUndone>(&it->payload)) {
            compensated.insert(undo->undone_event_id);
            continue;
        }
        if (const auto* point = std::get_if<PointAwarded>(&it->payload);
            point != nullptr && compensated.count(it->id) == 0) {
            // Only the newest point is ever undoable. A team-scoped undo
            // refuses here rather than reaching past the opponents' point,
            // which would silently rewrite a score nobody was looking at.
            if (only_team && point->team != *only_team) {
                return std::nullopt;
            }
            return it->id;
        }
    }
    return std::nullopt;
}

std::optional<PointAwarded> MatchEngine::next_undo_target() const {
    const std::optional<EventId> target = find_undo_target(std::nullopt);
    if (!target) {
        return std::nullopt;
    }
    const auto it = std::find_if(events_.begin(), events_.end(),
                                 [&](const StoredEvent& e) { return e.id == *target; });
    return std::get<PointAwarded>(it->payload);
}

DecidedEvent MatchEngine::make_decided(Event payload) const {
    return DecidedEvent{next_event_id_, events_.size() + 1, std::move(payload)};
}

MatchEngine::DecideResult MatchEngine::decide(const StartMatch& cmd) const {
    if (state_.lifecycle != MatchLifecycle::NotStarted) {
        return DecideResult::err(CommandError::MatchAlreadyStarted);
    }
    return DecideResult::ok(make_decided(MatchStarted{cmd.initial_serving_team}));
}

MatchEngine::DecideResult MatchEngine::decide(const AwardPoint& cmd) const {
    switch (state_.lifecycle) {
        case MatchLifecycle::Active:
            break;
        case MatchLifecycle::NotStarted:
            return DecideResult::err(CommandError::MatchNotStarted);
        case MatchLifecycle::Paused:
            return DecideResult::err(CommandError::MatchPausedError);
        case MatchLifecycle::Completed:
            return DecideResult::err(CommandError::MatchCompleted);
    }
    return DecideResult::ok(make_decided(PointAwarded{cmd.team, cmd.source}));
}

MatchEngine::DecideResult MatchEngine::decide(const UndoLastScoringAction& cmd) const {
    const std::optional<EventId> target = find_undo_target(cmd.only_team);
    if (!target) {
        return DecideResult::err(CommandError::NothingToUndo);
    }
    return DecideResult::ok(make_decided(ScoringActionUndone{*target}));
}

MatchEngine::DecideResult MatchEngine::decide(const SetServingTeam& cmd) const {
    if (state_.lifecycle == MatchLifecycle::Completed) {
        return DecideResult::err(CommandError::MatchCompleted);
    }
    return DecideResult::ok(make_decided(ServingTeamChanged{cmd.team}));
}

MatchEngine::DecideResult MatchEngine::decide(const PauseMatch&) const {
    if (state_.lifecycle != MatchLifecycle::Active) {
        return DecideResult::err(CommandError::MatchNotActive);
    }
    return DecideResult::ok(make_decided(MatchPaused{}));
}

MatchEngine::DecideResult MatchEngine::decide(const ResumeMatch&) const {
    if (state_.lifecycle != MatchLifecycle::Paused) {
        return DecideResult::err(CommandError::MatchNotActive);
    }
    return DecideResult::ok(make_decided(MatchResumed{}));
}

MatchEngine::DecideResult MatchEngine::decide(const FinishMatchManually& cmd) const {
    if (state_.lifecycle != MatchLifecycle::Active && state_.lifecycle != MatchLifecycle::Paused) {
        return DecideResult::err(CommandError::MatchNotActive);
    }
    return DecideResult::ok(make_decided(MatchFinishedManually{cmd.declared_winner}));
}

MatchEngine::DecideResult MatchEngine::decide(const ResetMatch&) const {
    return DecideResult::ok(make_decided(MatchReset{}));
}

EventId MatchEngine::commit(const DecidedEvent& decided) {
    // A stale DecidedEvent (another command committed in between) is a
    // programming error in the single-threaded application task.
    assert(decided.id == next_event_id_);

    if (std::holds_alternative<ScoringActionUndone>(decided.payload)) {
        next_event_id_++;
        events_.push_back(StoredEvent{decided.id, decided.payload});
        rebuild();
        return decided.id;
    }
    return append(decided.payload);
}

MatchEngine MatchEngine::replay(std::vector<StoredEvent> events, MatchConfig fallback_config) {
    MatchEngine engine;
    if (events.empty()) {
        engine.append(MatchCreated{1, fallback_config});
        return engine;
    }
    engine.events_ = std::move(events);
    EventId max_id = 0;
    for (const StoredEvent& stored : engine.events_) {
        max_id = std::max(max_id, stored.id);
    }
    engine.next_event_id_ = max_id + 1;
    engine.rebuild();
    return engine;
}

}  // namespace padel::domain
