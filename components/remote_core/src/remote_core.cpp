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

void RemoteCore::begin(std::uint32_t boot_id, std::uint32_t device_id) {
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
}

void RemoteCore::on_pair_assign(const protocol::PairAssignPacket& packet) {
    if (!advertising_ || packet.remote_id != settings_.remote_id) {
        return;
    }
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
    feedback_.play(FeedbackPattern::PressRegistered);
    start_intent();
}

void RemoteCore::persist_baseline_if_needed() {
    // Write-ahead: bump the persisted baseline before crossing it, so a
    // reboot can never reuse a sequence that was already sent.
    if (sequence_ + 1 > settings_.sequence_baseline) {
        settings_.sequence_baseline = sequence_ + 1 + config_.sequence_persist_chunk;
        store_.save(settings_);
    }
}

void RemoteCore::start_intent() {
    persist_baseline_if_needed();
    ++sequence_;

    Pending pending{};
    pending.packet.court_id = settings_.court_id;
    pending.packet.identity =
        protocol::IntentIdentity{settings_.remote_id, boot_id_, sequence_};
    pending.packet.team = settings_.team;
    pending.packet.battery_mv = battery_mv_;
    pending.packet.monotonic_ms = static_cast<std::uint32_t>(clock_.now_ms());
    pending_ = pending;
    ++stats_.intents_sent;
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

    // --- Debounce ---------------------------------------------------------
    if (raw_level_ != stable_level_) {
        const std::uint32_t required =
            raw_level_ ? config_.stable_press_ms : config_.stable_release_ms;
        if (now - level_since_ms_ >= required) {
            stable_level_ = raw_level_;
            if (stable_level_) {
                press_started_ms_ = now;
                on_debounced_press();
            } else if (pending_ && pending_->packet.button_duration_ms == 0) {
                pending_->packet.button_duration_ms = static_cast<std::uint16_t>(
                    now - press_started_ms_ > 0xFFFF ? 0xFFFF : now - press_started_ms_);
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
