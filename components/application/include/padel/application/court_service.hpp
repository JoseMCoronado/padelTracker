#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "padel/application/clock.hpp"
#include "padel/application/event_store.hpp"
#include "padel/common/ids.hpp"
#include "padel/common/result.hpp"
#include "padel/domain/match_engine.hpp"
#include "padel/protocol/dedup.hpp"
#include "padel/protocol/packets.hpp"

namespace padel::application {

struct CourtServiceConfig {
    CourtId court_id = 1;
    // Opposing-press pending window (spec section 12.4). Configurable; the
    // tradeoff is deliberate scoring latency vs conflict safety.
    std::uint32_t conflict_window_ms = 250;
};

enum class ServiceError : std::uint8_t {
    MatchNotActive,
    MatchAlreadyStarted,
    MatchNotStarted,
    MatchCompleted,
    MatchPaused,
    NothingToUndo,
    StorageFailure,
    NoConflictPending,
};

// Outcome of a locally sourced point (buttons/touch/simulator).
enum class LocalPointOutcome : std::uint8_t {
    Committed,              // durable and applied
    PendingConflictWindow,  // parked in the opposing-press window
    ConflictRaised,         // this press collided with an opposing one
    Rejected,               // validation rejection (see error)
    StorageFailure,         // could not commit durably; state unchanged
};

struct LocalPointResult {
    LocalPointOutcome outcome{};
    std::optional<ServiceError> error{};
};

// Details of an unresolved opposing-press conflict for the UI
// ("BOTH TEAMS PRESSED - SELECT WINNER").
struct ConflictInfo {
    std::uint64_t raised_at_ms = 0;
};

// The single authoritative entry point for the court (spec section 15):
// every input source converges on the same validated command path. Owns the
// match engine, dedup, remote allow-list, and conflict guard. Designed to be
// driven by one application task; ACKs to remotes are queued in an outbox the
// radio adapter drains.
//
// Point pipeline (spec sections 12.3-12.4, 13.3):
//   validate -> deduplicate -> conflict guard -> decide -> durable append ->
//   apply -> record dedup -> ACK
class CourtService {
public:
    CourtService(CourtServiceConfig config,
                 domain::MatchConfig match_config,
                 IEventStore& store,
                 const IClock& clock);

    // Boot recovery: rebuild engine state and dedup watermarks from journal
    // records recovered by the persistence layer (spec section 12.2).
    CourtService(CourtServiceConfig config,
                 domain::MatchConfig fallback_match_config,
                 std::vector<CommittedEvent> recovered,
                 IEventStore& store,
                 const IClock& clock);

    // --- Remote pairing (allow-list, spec section 10.8) ------------------
    void assign_remote(RemoteId remote_id, TeamId team);
    void unassign_remote(RemoteId remote_id);
    std::optional<TeamId> remote_team(RemoteId remote_id) const;

    // Per-remote liveness for the UI/diagnostics (spec 14.3, 14.9).
    struct RemoteInfo {
        RemoteId remote_id{};
        TeamId team{};
        bool ever_seen = false;
        std::uint64_t last_seen_ms = 0;
        std::uint16_t battery_mv = 0;  // 0 = unknown
    };
    std::optional<RemoteInfo> remote_info(TeamId team) const;

    // --- Point intents ----------------------------------------------------
    // Remote path. Never returns a value: terminal ACKs (including deferred
    // ones after the conflict window) appear in the outbox.
    void handle_point_intent(const protocol::PointIntentPacket& packet);

    // Local path (wired backup buttons, touchscreen admin, simulator).
    // PhysicalBackupButton participates in the conflict guard; organizer
    // sources (TouchscreenAdmin/CoordinatorOverride/Simulator) bypass it.
    LocalPointResult award_point_local(TeamId team, InputSource source);

    // Advance time-driven state: commits a pending press whose window
    // expired. Call from the application task loop.
    void tick();

    // --- Conflict resolution ----------------------------------------------
    bool conflict_pending() const { return conflict_.has_value(); }
    std::optional<ConflictInfo> conflict_info() const;
    // Organizer resolves: award to `winner`, or std::nullopt to cancel the
    // rally point entirely. Returns the awarded event id (nullopt on cancel).
    Result<std::optional<EventId>, ServiceError> resolve_conflict(std::optional<TeamId> winner);

    // --- Organizer / lifecycle commands (all journaled durably) -----------
    Result<EventId, ServiceError> start_match(TeamId initial_serving_team);
    // only_team restricts the undo to a point awarded to that team. Nothing
    // uses that today — remotes and the organizer both undo whichever point
    // came last (ADR-0014) — but the domain filter stays available.
    Result<EventId, ServiceError> undo_last_scoring_action(
        std::optional<TeamId> only_team = std::nullopt,
        InputSource source = InputSource::TouchscreenAdmin);
    Result<EventId, ServiceError> set_serving_team(TeamId team);
    Result<EventId, ServiceError> pause_match();
    Result<EventId, ServiceError> resume_match();
    Result<EventId, ServiceError> finish_match_manually(std::optional<TeamId> winner);
    Result<EventId, ServiceError> reset_match();

    // --- Observability ------------------------------------------------------
    const domain::MatchState& state() const { return engine_.state(); }
    std::optional<domain::PointAwarded> next_undo_target() const {
        return engine_.next_undo_target();
    }
    // Full event history, for projections that need more than the current
    // score (the post-match summary counts rallies from it).
    const std::vector<domain::StoredEvent>& journal() const { return engine_.journal(); }
    std::vector<protocol::AckPacket> drain_acks();
    const protocol::Deduplicator& deduplicator() const { return dedup_; }
    // Latched when a durable append fails; UI must surface it (spec 12.5).
    bool storage_fault() const { return storage_fault_; }
    void clear_storage_fault() { storage_fault_ = false; }

    struct Counters {
        std::uint32_t accepted = 0;
        std::uint32_t duplicates = 0;
        std::uint32_t rejected = 0;
        std::uint32_t conflicts = 0;
        std::uint32_t storage_failures = 0;
        std::uint32_t remote_undos = 0;
    };
    const Counters& counters() const { return counters_; }

private:
    struct PendingPress {
        TeamId team{};
        InputSource source{};
        std::optional<protocol::IntentIdentity> intent{};
        std::uint64_t deadline_ms = 0;
    };

    struct RemoteAssignment {
        RemoteId remote_id{};
        TeamId team{};
        bool used = false;
        bool ever_seen = false;
        std::uint64_t last_seen_ms = 0;
        std::uint16_t battery_mv = 0;
    };

    static constexpr std::size_t kMaxRemotes = 8;
    static constexpr std::size_t kRecentConflictIdentities = 8;

    // Remote hold-to-undo (ADR-0014): arrives on the POINT_INTENT frame with
    // Action::UndoLastPoint, after the paired/team/dedup checks. The sending
    // team is deliberately ignored — the last point goes back either way.
    void handle_undo_intent(const protocol::IntentIdentity& intent);

    // Shared commit path. Returns the ACK status the press earns and fills
    // event_id when a point was applied.
    protocol::AckStatus commit_point(TeamId team,
                                     InputSource source,
                                     const std::optional<protocol::IntentIdentity>& intent,
                                     std::optional<EventId>* event_id);

    template <typename Command>
    Result<EventId, ServiceError> run_command(const Command& cmd, InputSource source);

    void journal_genesis();
    void enqueue_ack(const protocol::IntentIdentity& intent, protocol::AckStatus status);
    void remember_conflicted(const protocol::IntentIdentity& intent);
    bool was_recently_conflicted(const protocol::IntentIdentity& intent) const;

    CourtServiceConfig config_{};
    domain::MatchEngine engine_;
    protocol::Deduplicator dedup_{};
    IEventStore& store_;
    const IClock& clock_;

    std::array<RemoteAssignment, kMaxRemotes> remotes_{};

    std::optional<PendingPress> pending_{};
    std::optional<ConflictInfo> conflict_{};
    // Identities terminally rejected with RejectedConflict; lost-ACK retries
    // are re-ACKed instead of reopening a pending window.
    std::array<protocol::IntentIdentity, kRecentConflictIdentities> recent_conflicted_{};
    std::size_t recent_conflicted_count_ = 0;
    std::size_t recent_conflicted_next_ = 0;

    std::vector<protocol::AckPacket> outbox_{};
    bool storage_fault_ = false;
    Counters counters_{};
};

}  // namespace padel::application
