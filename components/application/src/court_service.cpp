#include "padel/application/court_service.hpp"

#include "padel/common/log.hpp"
#include "padel/domain/projection.hpp"

namespace padel::application {
namespace {

char team_char(TeamId team) {
    return team == TeamId::A ? 'A' : 'B';
}

ServiceError to_service_error(domain::CommandError error) {
    switch (error) {
        case domain::CommandError::MatchNotActive:
            return ServiceError::MatchNotActive;
        case domain::CommandError::MatchAlreadyStarted:
            return ServiceError::MatchAlreadyStarted;
        case domain::CommandError::MatchNotStarted:
            return ServiceError::MatchNotStarted;
        case domain::CommandError::MatchCompleted:
            return ServiceError::MatchCompleted;
        case domain::CommandError::MatchPausedError:
            return ServiceError::MatchPaused;
        case domain::CommandError::NothingToUndo:
            return ServiceError::NothingToUndo;
    }
    return ServiceError::MatchNotActive;
}

protocol::AckStatus rejection_ack_status(domain::CommandError error) {
    switch (error) {
        case domain::CommandError::MatchPausedError:
            return protocol::AckStatus::RejectedPaused;
        case domain::CommandError::NothingToUndo:
            return protocol::AckStatus::RejectedNothingToUndo;
        case domain::CommandError::MatchNotStarted:
        case domain::CommandError::MatchCompleted:
        case domain::CommandError::MatchNotActive:
        default:
            return protocol::AckStatus::RejectedNotInMatch;
    }
}

bool organizer_source(InputSource source) {
    return source == InputSource::TouchscreenAdmin || source == InputSource::Simulator ||
           source == InputSource::CoordinatorOverride;
}

std::vector<domain::StoredEvent> to_stored_events(const std::vector<CommittedEvent>& recovered) {
    std::vector<domain::StoredEvent> events;
    events.reserve(recovered.size());
    for (const CommittedEvent& committed : recovered) {
        events.push_back(domain::StoredEvent{committed.event_id, committed.payload});
    }
    return events;
}

}  // namespace

CourtService::CourtService(CourtServiceConfig config,
                           domain::MatchConfig match_config,
                           IEventStore& store,
                           const IClock& clock)
    : config_(config), engine_(match_config), store_(store), clock_(clock) {
    journal_genesis();
}

CourtService::CourtService(CourtServiceConfig config,
                           domain::MatchConfig fallback_match_config,
                           std::vector<CommittedEvent> recovered,
                           IEventStore& store,
                           const IClock& clock)
    : config_(config),
      engine_(domain::MatchEngine::replay(to_stored_events(recovered), fallback_match_config)),
      store_(store),
      clock_(clock) {
    if (recovered.empty()) {
        // Nothing survived (fresh file): replay created a genesis event with
        // the fallback config; it must be journaled like in a fresh start.
        journal_genesis();
        return;
    }
    // Rebuild dedup watermarks so a retry of an already-journaled intent is
    // classified Duplicate after reboot (spec section 13.5, ADR-0007).
    for (const CommittedEvent& committed : recovered) {
        if (committed.intent) {
            dedup_.record(*committed.intent);
        }
    }
}

void CourtService::journal_genesis() {
    // The MatchCreated event is appended by the engine constructor; journal
    // it so replay reproduces the config and match id.
    const domain::StoredEvent& genesis = engine_.journal().front();
    CommittedEvent record{};
    record.event_id = genesis.id;
    record.match_id = engine_.state().match_id;
    record.state_revision = 1;
    record.payload = genesis.payload;
    record.source = InputSource::TouchscreenAdmin;
    record.monotonic_ms = clock_.now_ms();
    if (!store_.append(record)) {
        storage_fault_ = true;
        ++counters_.storage_failures;
        logging::emit(logging::Level::Error, "storage.commit_failed", "event=genesis");
    }
}

// --- Remote pairing ---------------------------------------------------------

void CourtService::assign_remote(RemoteId remote_id, TeamId team) {
    for (RemoteAssignment& slot : remotes_) {
        if (slot.used && slot.remote_id == remote_id) {
            slot.team = team;
            return;
        }
    }
    for (RemoteAssignment& slot : remotes_) {
        if (!slot.used) {
            slot = RemoteAssignment{remote_id, team, true};
            return;
        }
    }
}

void CourtService::unassign_remote(RemoteId remote_id) {
    for (RemoteAssignment& slot : remotes_) {
        if (slot.used && slot.remote_id == remote_id) {
            slot.used = false;
            return;
        }
    }
}

std::optional<TeamId> CourtService::remote_team(RemoteId remote_id) const {
    for (const RemoteAssignment& slot : remotes_) {
        if (slot.used && slot.remote_id == remote_id) {
            return slot.team;
        }
    }
    return std::nullopt;
}

std::optional<CourtService::RemoteInfo> CourtService::remote_info(TeamId team) const {
    for (const RemoteAssignment& slot : remotes_) {
        if (slot.used && slot.team == team) {
            return RemoteInfo{slot.remote_id, slot.team, slot.ever_seen, slot.last_seen_ms,
                              slot.battery_mv};
        }
    }
    return std::nullopt;
}

// --- ACK plumbing -----------------------------------------------------------

void CourtService::enqueue_ack(const protocol::IntentIdentity& intent,
                               protocol::AckStatus status) {
    protocol::AckPacket ack{};
    ack.court_id = config_.court_id;
    ack.identity = intent;
    ack.status = status;
    ack.state_revision = engine_.state().revision;
    ack.team_a_display_code = domain::display_code(engine_.state(), TeamId::A);
    ack.team_b_display_code = domain::display_code(engine_.state(), TeamId::B);
    outbox_.push_back(ack);
}

std::vector<protocol::AckPacket> CourtService::drain_acks() {
    std::vector<protocol::AckPacket> drained;
    drained.swap(outbox_);
    return drained;
}

void CourtService::remember_conflicted(const protocol::IntentIdentity& intent) {
    recent_conflicted_[recent_conflicted_next_] = intent;
    recent_conflicted_next_ = (recent_conflicted_next_ + 1) % kRecentConflictIdentities;
    if (recent_conflicted_count_ < kRecentConflictIdentities) {
        ++recent_conflicted_count_;
    }
}

bool CourtService::was_recently_conflicted(const protocol::IntentIdentity& intent) const {
    for (std::size_t i = 0; i < recent_conflicted_count_; ++i) {
        if (recent_conflicted_[i] == intent) {
            return true;
        }
    }
    return false;
}

// --- Shared commit path -----------------------------------------------------

protocol::AckStatus CourtService::commit_point(
    TeamId team,
    InputSource source,
    const std::optional<protocol::IntentIdentity>& intent,
    std::optional<EventId>* event_id) {
    const auto decided = engine_.decide(domain::AwardPoint{team, source});
    if (!decided) {
        ++counters_.rejected;
        return rejection_ack_status(decided.error());
    }

    CommittedEvent record{};
    record.event_id = decided.value().id;
    record.match_id = engine_.state().match_id;
    record.state_revision = decided.value().revision_after;
    record.payload = decided.value().payload;
    record.source = source;
    record.intent = intent;
    record.monotonic_ms = clock_.now_ms();

    if (!store_.append(record)) {
        // State untouched, dedup not recorded: a retry of this intent will be
        // processed as new once storage recovers (spec section 13.3).
        storage_fault_ = true;
        ++counters_.storage_failures;
        logging::emit(logging::Level::Error, "storage.commit_failed", "event=point team=%c",
                      team_char(team));
        return protocol::AckStatus::ErrorStorage;
    }

    engine_.commit(decided.value());
    if (intent) {
        dedup_.record(*intent);
    }
    ++counters_.accepted;
    logging::emit(logging::Level::Info, "match.point_accepted", "team=%c rev=%llu",
                  team_char(team),
                  static_cast<unsigned long long>(engine_.state().revision));
    if (event_id != nullptr) {
        *event_id = decided.value().id;
    }
    return protocol::AckStatus::Accepted;
}

// --- Remote point path ------------------------------------------------------

void CourtService::handle_point_intent(const protocol::PointIntentPacket& packet) {
    const protocol::IntentIdentity& intent = packet.identity;

    if (packet.court_id != config_.court_id) {
        ++counters_.rejected;
        enqueue_ack(intent, protocol::AckStatus::RejectedInvalidPacket);
        return;
    }

    const std::optional<TeamId> assigned = remote_team(intent.remote_id);
    if (!assigned) {
        ++counters_.rejected;
        logging::emit(logging::Level::Warn, "radio.rejected", "reason=unpaired remote=0x%X",
                      static_cast<unsigned>(intent.remote_id));
        enqueue_ack(intent, protocol::AckStatus::RejectedUnpaired);
        return;
    }
    for (RemoteAssignment& slot : remotes_) {
        if (slot.used && slot.remote_id == intent.remote_id) {
            slot.ever_seen = true;
            slot.last_seen_ms = clock_.now_ms();
            if (packet.battery_mv != 0) {
                slot.battery_mv = packet.battery_mv;
            }
        }
    }
    if (*assigned != packet.team) {
        ++counters_.rejected;
        enqueue_ack(intent, protocol::AckStatus::RejectedWrongTeam);
        return;
    }

    // Lost-ACK retry of a press that was terminally rejected as a conflict:
    // re-ACK, never reopen a pending window for it.
    if (was_recently_conflicted(intent)) {
        enqueue_ack(intent, protocol::AckStatus::RejectedConflict);
        return;
    }

    switch (dedup_.classify(intent)) {
        case protocol::DedupResult::New:
            break;
        case protocol::DedupResult::Duplicate:
            ++counters_.duplicates;
            logging::emit(logging::Level::Info, "radio.duplicate", "remote=0x%X seq=%u",
                          static_cast<unsigned>(intent.remote_id),
                          static_cast<unsigned>(intent.sequence));
            enqueue_ack(intent, protocol::AckStatus::DuplicateAccepted);
            return;
        case protocol::DedupResult::Stale:
            ++counters_.rejected;
            enqueue_ack(intent, protocol::AckStatus::RejectedInvalidPacket);
            return;
    }

    if (packet.action == protocol::Action::UndoLastPoint) {
        handle_undo_intent(intent, packet.team);
        return;
    }

    // Reject before parking: a press while paused/finished must not sit in
    // the window and commit later.
    if (const auto dry_run = engine_.decide(domain::AwardPoint{packet.team, InputSource::Remote});
        !dry_run) {
        ++counters_.rejected;
        enqueue_ack(intent, rejection_ack_status(dry_run.error()));
        return;
    }

    if (conflict_) {
        // Unresolved conflict on screen: presses are part of the ambiguity.
        ++counters_.rejected;
        remember_conflicted(intent);
        enqueue_ack(intent, protocol::AckStatus::RejectedConflict);
        return;
    }

    if (pending_) {
        if (pending_->intent && *pending_->intent == intent) {
            return;  // retry of the parked press; terminal ACK comes at commit
        }
        if (pending_->team == packet.team) {
            // A second distinct same-team press: commit the parked one now,
            // then park this press in a fresh window.
            const PendingPress parked = *pending_;
            pending_.reset();
            const protocol::AckStatus status =
                commit_point(parked.team, parked.source, parked.intent, nullptr);
            if (parked.intent) {
                enqueue_ack(*parked.intent, status);
            }
        } else {
            // Opposing press inside the window: neither commits automatically
            // (spec section 12.4).
            ++counters_.conflicts;
            if (pending_->intent) {
                remember_conflicted(*pending_->intent);
                enqueue_ack(*pending_->intent, protocol::AckStatus::RejectedConflict);
            }
            remember_conflicted(intent);
            enqueue_ack(intent, protocol::AckStatus::RejectedConflict);
            pending_.reset();
            conflict_ = ConflictInfo{clock_.now_ms()};
            logging::emit(logging::Level::Warn, "match.conflict_opened", "window=%ums",
                          static_cast<unsigned>(config_.conflict_window_ms));
            return;
        }
    }

    if (config_.conflict_window_ms == 0) {
        // First-press-wins policy (window disabled).
        std::optional<EventId> event_id{};
        const protocol::AckStatus status =
            commit_point(packet.team, InputSource::Remote, intent, &event_id);
        enqueue_ack(intent, status);
        return;
    }

    pending_ = PendingPress{packet.team, InputSource::Remote, intent,
                            clock_.now_ms() + config_.conflict_window_ms};
}

void CourtService::handle_undo_intent(const protocol::IntentIdentity& intent, TeamId team) {
    // While a press is parked or a conflict is unresolved, the score the
    // player is reacting to is not settled yet; taking a point back now would
    // race the commit that is about to land.
    if (conflict_ || pending_) {
        ++counters_.rejected;
        // Remembered so a retry of this identity gets the same terminal
        // answer instead of succeeding once the window happens to expire.
        remember_conflicted(intent);
        enqueue_ack(intent, protocol::AckStatus::RejectedConflict);
        return;
    }

    const auto result = undo_last_scoring_action(team, InputSource::Remote);
    if (!result) {
        ++counters_.rejected;
        enqueue_ack(intent, result.error() == ServiceError::StorageFailure
                                ? protocol::AckStatus::ErrorStorage
                                : protocol::AckStatus::RejectedNothingToUndo);
        return;
    }
    // Recording only after a durable append means a retry of a lost ACK
    // re-ACKs as a duplicate instead of undoing a second point.
    dedup_.record(intent);
    ++counters_.accepted;
    ++counters_.remote_undos;
    enqueue_ack(intent, protocol::AckStatus::Accepted);
}

// --- Local point path -------------------------------------------------------

LocalPointResult CourtService::award_point_local(TeamId team, InputSource source) {
    if (const auto dry_run = engine_.decide(domain::AwardPoint{team, source}); !dry_run) {
        ++counters_.rejected;
        return LocalPointResult{LocalPointOutcome::Rejected,
                                to_service_error(dry_run.error())};
    }

    const bool bypass_guard = organizer_source(source);
    if (!bypass_guard) {
        if (conflict_) {
            return LocalPointResult{LocalPointOutcome::ConflictRaised, std::nullopt};
        }
        if (pending_) {
            if (pending_->team == team) {
                const PendingPress parked = *pending_;
                pending_.reset();
                const protocol::AckStatus status =
                    commit_point(parked.team, parked.source, parked.intent, nullptr);
                if (parked.intent) {
                    enqueue_ack(*parked.intent, status);
                }
            } else {
                ++counters_.conflicts;
                if (pending_->intent) {
                    remember_conflicted(*pending_->intent);
                    enqueue_ack(*pending_->intent, protocol::AckStatus::RejectedConflict);
                }
                pending_.reset();
                conflict_ = ConflictInfo{clock_.now_ms()};
                logging::emit(logging::Level::Warn, "match.conflict_opened", "window=%ums",
                              static_cast<unsigned>(config_.conflict_window_ms));
                return LocalPointResult{LocalPointOutcome::ConflictRaised, std::nullopt};
            }
        }
        if (config_.conflict_window_ms > 0) {
            pending_ = PendingPress{team, source, std::nullopt,
                                    clock_.now_ms() + config_.conflict_window_ms};
            return LocalPointResult{LocalPointOutcome::PendingConflictWindow, std::nullopt};
        }
    }

    const protocol::AckStatus status = commit_point(team, source, std::nullopt, nullptr);
    switch (status) {
        case protocol::AckStatus::Accepted:
            return LocalPointResult{LocalPointOutcome::Committed, std::nullopt};
        case protocol::AckStatus::ErrorStorage:
            return LocalPointResult{LocalPointOutcome::StorageFailure,
                                    ServiceError::StorageFailure};
        default:
            return LocalPointResult{LocalPointOutcome::Rejected,
                                    ServiceError::MatchNotActive};
    }
}

// --- Time-driven state ------------------------------------------------------

void CourtService::tick() {
    if (!pending_ || clock_.now_ms() < pending_->deadline_ms) {
        return;
    }
    const PendingPress parked = *pending_;
    pending_.reset();
    const protocol::AckStatus status =
        commit_point(parked.team, parked.source, parked.intent, nullptr);
    if (parked.intent) {
        enqueue_ack(*parked.intent, status);
    }
}

// --- Conflict resolution ----------------------------------------------------

std::optional<ConflictInfo> CourtService::conflict_info() const {
    return conflict_;
}

Result<std::optional<EventId>, ServiceError> CourtService::resolve_conflict(
    std::optional<TeamId> winner) {
    using R = Result<std::optional<EventId>, ServiceError>;
    if (!conflict_) {
        return R::err(ServiceError::NoConflictPending);
    }
    if (!winner) {
        conflict_.reset();
        logging::emit(logging::Level::Info, "match.conflict_resolved", "outcome=cancelled");
        return R::ok(std::nullopt);
    }

    std::optional<EventId> event_id{};
    const protocol::AckStatus status =
        commit_point(*winner, InputSource::TouchscreenAdmin, std::nullopt, &event_id);
    if (status == protocol::AckStatus::Accepted) {
        conflict_.reset();
        logging::emit(logging::Level::Info, "match.conflict_resolved", "outcome=team_%c",
                      team_char(*winner));
        return R::ok(event_id);
    }
    if (status == protocol::AckStatus::ErrorStorage) {
        // Keep the conflict raised so the organizer can retry.
        return R::err(ServiceError::StorageFailure);
    }
    ++counters_.rejected;
    conflict_.reset();
    return R::err(ServiceError::MatchNotActive);
}

// --- Organizer / lifecycle commands -----------------------------------------

template <typename Command>
Result<EventId, ServiceError> CourtService::run_command(const Command& cmd, InputSource source) {
    using R = Result<EventId, ServiceError>;
    const auto decided = engine_.decide(cmd);
    if (!decided) {
        return R::err(to_service_error(decided.error()));
    }

    CommittedEvent record{};
    record.event_id = decided.value().id;
    record.match_id = engine_.state().match_id;
    record.state_revision = decided.value().revision_after;
    record.payload = decided.value().payload;
    record.source = source;
    record.monotonic_ms = clock_.now_ms();

    if (!store_.append(record)) {
        storage_fault_ = true;
        ++counters_.storage_failures;
        logging::emit(logging::Level::Error, "storage.commit_failed", "event=command");
        return R::err(ServiceError::StorageFailure);
    }
    return R::ok(engine_.commit(decided.value()));
}

Result<EventId, ServiceError> CourtService::start_match(TeamId initial_serving_team) {
    auto result =
        run_command(domain::StartMatch{initial_serving_team}, InputSource::TouchscreenAdmin);
    if (result) {
        logging::emit(logging::Level::Info, "match.started", "serving=%c",
                      team_char(initial_serving_team));
    }
    return result;
}

Result<EventId, ServiceError> CourtService::undo_last_scoring_action(
    std::optional<TeamId> only_team, InputSource source) {
    auto result = run_command(domain::UndoLastScoringAction{source, only_team}, source);
    if (result) {
        logging::emit(logging::Level::Info, "match.undo", "team=%s rev=%llu",
                      only_team ? (*only_team == TeamId::A ? "A" : "B") : "any",
                      static_cast<unsigned long long>(engine_.state().revision));
    }
    return result;
}

Result<EventId, ServiceError> CourtService::set_serving_team(TeamId team) {
    return run_command(domain::SetServingTeam{team}, InputSource::TouchscreenAdmin);
}

Result<EventId, ServiceError> CourtService::pause_match() {
    return run_command(domain::PauseMatch{}, InputSource::TouchscreenAdmin);
}

Result<EventId, ServiceError> CourtService::resume_match() {
    return run_command(domain::ResumeMatch{}, InputSource::TouchscreenAdmin);
}

Result<EventId, ServiceError> CourtService::finish_match_manually(std::optional<TeamId> winner) {
    return run_command(domain::FinishMatchManually{winner}, InputSource::TouchscreenAdmin);
}

Result<EventId, ServiceError> CourtService::reset_match() {
    auto result = run_command(domain::ResetMatch{}, InputSource::TouchscreenAdmin);
    if (result) {
        logging::emit(logging::Level::Warn, "match.reset", "%s", "");
    }
    return result;
}

}  // namespace padel::application
