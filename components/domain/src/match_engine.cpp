#include "padel/domain/match_engine.hpp"

#include <algorithm>
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

std::optional<EventId> MatchEngine::find_undo_target() const {
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
        if (std::holds_alternative<PointAwarded>(it->payload) &&
            compensated.count(it->id) == 0) {
            return it->id;
        }
    }
    return std::nullopt;
}

std::optional<PointAwarded> MatchEngine::next_undo_target() const {
    const std::optional<EventId> target = find_undo_target();
    if (!target) {
        return std::nullopt;
    }
    const auto it = std::find_if(events_.begin(), events_.end(),
                                 [&](const StoredEvent& e) { return e.id == *target; });
    return std::get<PointAwarded>(it->payload);
}

MatchEngine::CommandResult MatchEngine::handle(const StartMatch& cmd) {
    if (state_.lifecycle != MatchLifecycle::NotStarted) {
        return CommandResult::err(CommandError::MatchAlreadyStarted);
    }
    return CommandResult::ok(append(MatchStarted{cmd.initial_serving_team}));
}

MatchEngine::CommandResult MatchEngine::handle(const AwardPoint& cmd) {
    switch (state_.lifecycle) {
        case MatchLifecycle::Active:
            break;
        case MatchLifecycle::NotStarted:
            return CommandResult::err(CommandError::MatchNotStarted);
        case MatchLifecycle::Paused:
            return CommandResult::err(CommandError::MatchPausedError);
        case MatchLifecycle::Completed:
            return CommandResult::err(CommandError::MatchCompleted);
    }
    return CommandResult::ok(append(PointAwarded{cmd.team, cmd.source}));
}

MatchEngine::CommandResult MatchEngine::handle(const UndoLastScoringAction&) {
    const std::optional<EventId> target = find_undo_target();
    if (!target) {
        return CommandResult::err(CommandError::NothingToUndo);
    }
    const EventId id = next_event_id_++;
    events_.push_back(StoredEvent{id, ScoringActionUndone{*target}});
    rebuild();
    return CommandResult::ok(id);
}

MatchEngine::CommandResult MatchEngine::handle(const SetServingTeam& cmd) {
    if (state_.lifecycle == MatchLifecycle::Completed) {
        return CommandResult::err(CommandError::MatchCompleted);
    }
    return CommandResult::ok(append(ServingTeamChanged{cmd.team}));
}

MatchEngine::CommandResult MatchEngine::handle(const PauseMatch&) {
    if (state_.lifecycle != MatchLifecycle::Active) {
        return CommandResult::err(CommandError::MatchNotActive);
    }
    return CommandResult::ok(append(MatchPaused{}));
}

MatchEngine::CommandResult MatchEngine::handle(const ResumeMatch&) {
    if (state_.lifecycle != MatchLifecycle::Paused) {
        return CommandResult::err(CommandError::MatchNotActive);
    }
    return CommandResult::ok(append(MatchResumed{}));
}

MatchEngine::CommandResult MatchEngine::handle(const FinishMatchManually& cmd) {
    if (state_.lifecycle != MatchLifecycle::Active && state_.lifecycle != MatchLifecycle::Paused) {
        return CommandResult::err(CommandError::MatchNotActive);
    }
    return CommandResult::ok(append(MatchFinishedManually{cmd.declared_winner}));
}

MatchEngine::CommandResult MatchEngine::handle(const ResetMatch&) {
    return CommandResult::ok(append(MatchReset{}));
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
