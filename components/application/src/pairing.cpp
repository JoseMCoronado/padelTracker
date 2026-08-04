#include "padel/application/pairing.hpp"

#include <cstdio>

#include "padel/common/log.hpp"

namespace padel::application {

PairingService::PairingService(Config config, CourtService& court, ISettings& settings,
                               const IClock& clock)
    : config_(config), court_(court), settings_(settings), clock_(clock) {}

void PairingService::load_assignments() {
    assignments_ = settings_.load_assignments();
    for (const StoredAssignment& assignment : assignments_) {
        court_.assign_remote(assignment.remote_id, assignment.team);
    }
}

void PairingService::begin(TeamId team) {
    session_ = Session{team, clock_.now_ms() + config_.window_ms};
    candidate_.reset();
    logging::emit(logging::Level::Info, "pair.window_opened", "team=%c",
                  team == TeamId::A ? 'A' : 'B');
}

void PairingService::cancel() {
    session_.reset();
    candidate_.reset();
}

void PairingService::tick() {
    if (session_ && clock_.now_ms() >= session_->deadline_ms) {
        logging::emit(logging::Level::Info, "pair.window_expired", "%s", "");
        cancel();
    }
}

std::optional<TeamId> PairingService::team() const {
    if (!session_) {
        return std::nullopt;
    }
    return session_->team;
}

std::uint32_t PairingService::seconds_left() const {
    if (!session_) {
        return 0;
    }
    const std::uint64_t now = clock_.now_ms();
    return now >= session_->deadline_ms
               ? 0
               : static_cast<std::uint32_t>((session_->deadline_ms - now) / 1000);
}

void PairingService::handle_pair_request(const protocol::PairRequestPacket& packet) {
    if (!session_) {
        return;
    }
    // Never silently replace an existing assignment (spec 10.8): a remote
    // that already belongs to a team must be unassigned by the organizer
    // before it can pair again — unless it is re-pairing to the same team.
    const std::optional<TeamId> current = court_.remote_team(packet.remote_id);
    if (current && *current != session_->team) {
        logging::emit(logging::Level::Warn, "pair.request_blocked",
                      "remote=0x%X already_assigned=%c",
                      static_cast<unsigned>(packet.remote_id),
                      *current == TeamId::A ? 'A' : 'B');
        return;
    }

    Candidate candidate{};
    candidate.remote_id = packet.remote_id;
    candidate.battery_mv = packet.battery_mv;
    char short_id[8];
    std::snprintf(short_id, sizeof(short_id), "%04X",
                  static_cast<unsigned>(packet.remote_id & 0xFFFF));
    candidate.short_id = short_id;
    candidate_ = candidate;
    logging::emit(logging::Level::Info, "pair.candidate", "remote=0x%X",
                  static_cast<unsigned>(packet.remote_id));
}

std::optional<protocol::PairAssignPacket> PairingService::confirm() {
    if (!session_ || !candidate_) {
        return std::nullopt;
    }
    const TeamId team = session_->team;
    const std::uint32_t remote_id = candidate_->remote_id;

    // Replace any previous assignment of this remote, and any previous
    // remote on this team slot is kept (a team may have several remotes;
    // the allow-list supports it).
    court_.assign_remote(remote_id, team);
    bool updated = false;
    for (StoredAssignment& assignment : assignments_) {
        if (assignment.remote_id == remote_id) {
            assignment.team = team;
            updated = true;
        }
    }
    if (!updated) {
        assignments_.push_back(StoredAssignment{remote_id, team});
    }
    persist();

    protocol::PairAssignPacket assign{};
    assign.court_id = config_.court_id;
    assign.remote_id = remote_id;
    assign.team = team;
    assign.channel = config_.channel;

    logging::emit(logging::Level::Info, "pair.confirmed", "remote=0x%X team=%c",
                  static_cast<unsigned>(remote_id), team == TeamId::A ? 'A' : 'B');
    cancel();
    return assign;
}

void PairingService::unassign(RemoteId remote_id) {
    court_.unassign_remote(remote_id);
    for (std::size_t i = 0; i < assignments_.size(); ++i) {
        if (assignments_[i].remote_id == remote_id) {
            assignments_.erase(assignments_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    persist();
}

void PairingService::persist() const {
    settings_.save_assignments(assignments_);
}

}  // namespace padel::application
