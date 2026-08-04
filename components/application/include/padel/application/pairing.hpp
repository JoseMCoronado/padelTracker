#pragma once

#include <optional>
#include <string>
#include <vector>

#include "padel/application/clock.hpp"
#include "padel/application/court_service.hpp"
#include "padel/application/settings.hpp"
#include "padel/protocol/packets.hpp"

namespace padel::application {

// Court-side physical-presence pairing flow (spec 10.8 / 14.5):
//
//   organizer opens a window for one team -> remote in pairing mode
//   broadcasts PAIR_REQUEST -> court surfaces the short device id ->
//   organizer confirms -> assignment persisted + PAIR_ASSIGN transmitted.
//
// A remote already assigned to the other team is never silently replaced:
// its requests are ignored while it holds an assignment (the organizer must
// unassign first).
class PairingService {
public:
    struct Config {
        CourtId court_id = 1;
        std::uint8_t channel = 1;
        std::uint32_t window_ms = 30'000;
    };

    struct Candidate {
        std::uint32_t remote_id = 0;
        std::uint16_t battery_mv = 0;
        std::string short_id;  // low 16 bits, hex, for the confirmation UI
    };

    PairingService(Config config, CourtService& court, ISettings& settings,
                   const IClock& clock);

    // Restores persisted assignments into the court allow-list (boot).
    void load_assignments();

    void begin(TeamId team);
    void cancel();
    // Expires the window; call from the app loop.
    void tick();

    bool active() const { return session_.has_value(); }
    std::optional<TeamId> team() const;
    std::uint32_t seconds_left() const;
    const std::optional<Candidate>& candidate() const { return candidate_; }

    // Radio inbound while the window is open.
    void handle_pair_request(const protocol::PairRequestPacket& packet);

    // Organizer confirmation. On success: assigns, persists, and returns the
    // PAIR_ASSIGN packet for the radio to transmit (broadcast; the remote
    // filters by remote_id).
    std::optional<protocol::PairAssignPacket> confirm();

    // Removes an assignment (persisted immediately).
    void unassign(RemoteId remote_id);

private:
    void persist() const;

    struct Session {
        TeamId team{TeamId::A};
        std::uint64_t deadline_ms = 0;
    };

    Config config_;
    CourtService& court_;
    ISettings& settings_;
    const IClock& clock_;

    std::optional<Session> session_{};
    std::optional<Candidate> candidate_{};
    std::vector<StoredAssignment> assignments_{};
};

}  // namespace padel::application
