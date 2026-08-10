#include "padel/remote/remote_core.hpp"

namespace padel::remote {

FeedbackPattern feedback_for(protocol::AckStatus status) {
    switch (status) {
        case protocol::AckStatus::Accepted:
        case protocol::AckStatus::DuplicateAccepted:
            return FeedbackPattern::Accepted;
        case protocol::AckStatus::RejectedConflict:
            return FeedbackPattern::RejectedConflict;
        case protocol::AckStatus::RejectedNotInMatch:
        case protocol::AckStatus::RejectedWrongTeam:
        case protocol::AckStatus::RejectedUnpaired:
        case protocol::AckStatus::RejectedPaused:
        case protocol::AckStatus::RejectedInvalidPacket:
        case protocol::AckStatus::RejectedNothingToUndo:
            return FeedbackPattern::RejectedOther;
        case protocol::AckStatus::ErrorStorage:
            return FeedbackPattern::CommFailed;
    }
    return FeedbackPattern::RejectedOther;
}

RemoteCore::RemoteCore(RemoteCoreConfig config,
                       const IClock& clock,
                       IRadio& radio,
                       IFeedback& feedback,
                       ISettingsStore& store)
    : config_(config), clock_(clock), radio_(radio), feedback_(feedback), store_(store) {}

void RemoteCore::begin(std::uint32_t boot_id, std::uint32_t device_id,
                       bool woke_from_sleep) {
    boot_id_ = boot_id;
    if (const auto loaded = store_.load()) {
        settings_ = *loaded;
    }
    if (settings_.remote_id == 0) {
        settings_.remote_id = device_id;
    }
    // Resume past any identity that may have been used before the reboot
    // (spec 11.5). The fresh random boot_id already isolates identities, but
    // the baseline guards against boot_id collisions.
    sequence_ = settings_.sequence_baseline;
    level_since_ms_ = clock_.now_ms();
    note_activity();
    woke_from_sleep_ = woke_from_sleep;
}

void RemoteCore::note_activity() {
    last_activity_ms_ = clock_.now_ms();
    woke_from_sleep_ = false;
}

bool RemoteCore::sleep_due() const {
    // Anything outstanding keeps the remote awake: an unacknowledged intent,
    // an active pairing advertisement, or a finger still on the button.
    if (pending_ || advertising_ || press_armed_ || stable_level_ || raw_level_) {
        return false;
    }
    const RemoteState current = state();
    // An unpaired remote in a drawer is exactly the case worth sleeping, so
    // PairingRequired sleeps too.
    if (current != RemoteState::Ready && current != RemoteState::PairingRequired) {
        return false;
    }
    // A wake nothing followed buys the shorter of the two windows, never a
    // longer one: shortening inactivity_sleep_ms for bench work would
    // otherwise make a spurious wake stay awake longer than an idle remote.
    std::uint32_t timeout = config_.inactivity_sleep_ms;
    if (woke_from_sleep_ && config_.post_wake_idle_ms < timeout) {
        timeout = config_.post_wake_idle_ms;
    }
    return clock_.now_ms() - last_activity_ms_ >= timeout;
}

void RemoteCore::apply_pairing(std::uint32_t remote_id, CourtId court_id, TeamId team) {
    settings_.paired = true;
    settings_.remote_id = remote_id;
    settings_.court_id = court_id;
    settings_.team = team;
    store_.save(settings_);
    feedback_.play(FeedbackPattern::PairingSuccess);
}

void RemoteCore::clear_pairing() {
    settings_.paired = false;
    store_.save(settings_);
}

RemoteState RemoteCore::state() const {
    if (advertising_) {
        return RemoteState::PairingAdvertise;
    }
    if (!settings_.paired) {
        return RemoteState::PairingRequired;
    }
    return pending_ ? RemoteState::PendingIntent : RemoteState::Ready;
}

void RemoteCore::enter_pairing_mode() {
    const std::uint64_t now = clock_.now_ms();
    advertising_ = true;
    advertise_until_ms_ = now + config_.pairing_timeout_ms;
    next_advertise_ms_ = now;  // first broadcast immediately
    pending_.reset();
    // The gesture that got us here must not also score when the finger lifts.
    press_armed_ = false;
}

void RemoteCore::on_pair_assign(const protocol::PairAssignPacket& packet) {
    if (!advertising_ || packet.remote_id != settings_.remote_id) {
        return;
    }
    note_activity();
    advertising_ = false;
    apply_pairing(settings_.remote_id, packet.court_id, packet.team);
}

void RemoteCore::set_button_level(bool pressed) {
    if (pressed != raw_level_) {
        raw_level_ = pressed;
        level_since_ms_ = clock_.now_ms();
    }
}

void RemoteCore::on_debounced_press() {
    const std::uint64_t now = clock_.now_ms();
    press_armed_ = false;
    // Every real press counts as activity, including one the guard goes on to
    // suppress: the finger was there either way.
    note_activity();

    // Local retrigger guard: a second accepted press within the guard window
    // is suppressed (spec 11.2: double taps must not double-score).
    if (last_accepted_press_ms_ != 0 &&
        now - last_accepted_press_ms_ < config_.retrigger_guard_ms) {
        ++stats_.presses_suppressed;
        return;
    }
    last_accepted_press_ms_ = now;
    ++stats_.presses;

    if (!settings_.paired) {
        feedback_.play(FeedbackPattern::PairingRequired);
        return;
    }
    // Stop-and-wait: never a new sequence while one intent is in flight.
    if (pending_) {
        ++stats_.presses_suppressed;
        return;
    }
    // The cue fires now so the button feels instant, but the packet waits for
    // the release: until the finger lifts we cannot tell a point from a hold
    // that is on its way to becoming an undo (ADR-0014).
    feedback_.play(FeedbackPattern::PressRegistered);
    press_armed_ = true;
}

void RemoteCore::on_debounced_release(std::uint64_t now) {
    if (!press_armed_) {
        return;  // suppressed press, or the hold already spent itself on an undo
    }
    press_armed_ = false;
    start_intent(protocol::Action::AwardPoint, now - press_started_ms_);
}

void RemoteCore::persist_baseline_if_needed() {
    // Write-ahead: bump the persisted baseline before crossing it, so a
    // reboot can never reuse a sequence that was already sent.
    if (sequence_ + 1 > settings_.sequence_baseline) {
        settings_.sequence_baseline = sequence_ + 1 + config_.sequence_persist_chunk;
        store_.save(settings_);
    }
}

void RemoteCore::start_intent(protocol::Action action, std::uint64_t held_ms) {
    persist_baseline_if_needed();
    ++sequence_;

    Pending pending{};
    pending.packet.court_id = settings_.court_id;
    pending.packet.identity =
        protocol::IntentIdentity{settings_.remote_id, boot_id_, sequence_};
    pending.packet.team = settings_.team;
    pending.packet.action = action;
    // Known before the first transmission, so retries of the same identity
    // always carry identical bytes.
    pending.packet.button_duration_ms =
        static_cast<std::uint16_t>(held_ms > 0xFFFF ? 0xFFFF : held_ms);
    pending.packet.battery_mv = battery_mv_;
    pending.packet.monotonic_ms = static_cast<std::uint32_t>(clock_.now_ms());
    pending_ = pending;
    ++stats_.intents_sent;
    if (action == protocol::Action::UndoLastPoint) {
        ++stats_.undos_sent;
    }
    transmit();
}

void RemoteCore::transmit() {
    Pending& p = *pending_;
    ++p.attempts;
    radio_.send_intent(p.packet);
    p.awaiting_ack = true;
    p.deadline_ms = clock_.now_ms() + config_.ack_timeout_ms;
}

void RemoteCore::on_ack(const protocol::AckPacket& ack) {
    if (!pending_ || !(ack.identity == pending_->packet.identity)) {
        return;  // stale or foreign ACK
    }
    note_activity();
    const FeedbackPattern pattern = feedback_for(ack.status);
    if (pattern == FeedbackPattern::Accepted) {
        ++stats_.confirmed;
    } else {
        ++stats_.rejected;
    }
    feedback_.play(pattern);
    pending_.reset();
}

void RemoteCore::poll() {
    const std::uint64_t now = clock_.now_ms();

    // --- Pairing advertise (spec 11.1) --------------------------------------
    if (advertising_) {
        if (now >= advertise_until_ms_) {
            advertising_ = false;  // timed out; back to PairingRequired
        } else if (now >= next_advertise_ms_) {
            protocol::PairRequestPacket request{};
            request.remote_id = settings_.remote_id;
            request.boot_id = boot_id_;
            request.fw_version = 1;
            request.battery_mv = battery_mv_;
            radio_.send_pair_request(request);
            next_advertise_ms_ = now + config_.pairing_advertise_interval_ms;
        }
    }

    // Long hold while unpaired enters pairing mode (deliberate gesture,
    // spec 11.2).
    if (!settings_.paired && !advertising_ && stable_level_ &&
        now - press_started_ms_ >= config_.pairing_hold_ms) {
        enter_pairing_mode();
    }

    // Hold while paired takes the last point back, whichever team scored it
    // (ADR-0014).
    // Once per hold: a second undo needs a fresh press.
    if (settings_.paired && stable_level_ && press_armed_ &&
        now - press_started_ms_ >= config_.undo_hold_ms) {
        press_armed_ = false;
        feedback_.play(FeedbackPattern::UndoSent);
        start_intent(protocol::Action::UndoLastPoint, now - press_started_ms_);
    }

    // --- Debounce ---------------------------------------------------------
    if (raw_level_ != stable_level_) {
        const std::uint32_t required =
            raw_level_ ? config_.stable_press_ms : config_.stable_release_ms;
        if (now - level_since_ms_ >= required) {
            stable_level_ = raw_level_;
            if (stable_level_) {
                // Time the hold from when the finger actually landed, not
                // from when the debounce believed it: the undo and pairing
                // gestures are what the player counts out loud, and they
                // must not stretch when stable_press_ms is tuned up.
                press_started_ms_ = level_since_ms_;
                on_debounced_press();
            } else {
                on_debounced_release(now);
            }
        }
    }

    // --- Retry / timeout ----------------------------------------------------
    if (pending_ && now >= pending_->deadline_ms) {
        Pending& p = *pending_;
        if (p.awaiting_ack) {
            if (p.attempts >= config_.max_attempts) {
                feedback_.play(FeedbackPattern::CommFailed);
                ++stats_.failed;
                pending_.reset();
            } else {
                // Back off before the retransmission (same identity).
                p.awaiting_ack = false;
                p.deadline_ms = now + config_.backoff_ms[p.attempts < 5 ? p.attempts : 4];
            }
        } else {
            ++stats_.retries;
            transmit();
        }
    }
}

}  // namespace padel::remote
