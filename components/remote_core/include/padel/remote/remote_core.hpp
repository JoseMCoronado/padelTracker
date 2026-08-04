#pragma once

#include <cstdint>
#include <optional>

#include "padel/common/ids.hpp"
#include "padel/protocol/packets.hpp"

// Portable remote logic (spec sections 11.1-11.5): debounce, the
// stop-and-wait intent lifecycle with retries, the centralized feedback
// table, and the persistence contract. No hardware dependencies — the XIAO
// firmware wires these interfaces to ESP-NOW / GPIO / LED / NVS, and native
// tests drive them directly.
namespace padel::remote {

// --- Injected platform interfaces -------------------------------------------

class IClock {
public:
    virtual ~IClock() = default;
    virtual std::uint64_t now_ms() const = 0;
};

class IRadio {
public:
    virtual ~IRadio() = default;
    // Best-effort transmit; delivery/loss is the protocol layer's problem.
    virtual void send_intent(const protocol::PointIntentPacket& packet) = 0;
    // Broadcast while in pairing mode (spec 10.8). Default no-op so simple
    // test radios only implement what they exercise.
    virtual void send_pair_request(const protocol::PairRequestPacket& packet) { (void)packet; }
};

// Centralized feedback contract (spec 11.3). One pattern per outcome; the
// firmware maps patterns to LED/haptic timings in exactly one module.
enum class FeedbackPattern : std::uint8_t {
    PressRegistered,   // immediate cue that the press was captured
    Accepted,          // one short pulse / green flash
    RejectedConflict,  // two short pulses / amber-red alternating
    RejectedOther,     // two medium pulses / amber
    CommFailed,        // one long pulse / three red flashes
    PairingRequired,   // press with no credentials
    PairingSuccess,    // three short pulses / green sequence
};

class IFeedback {
public:
    virtual ~IFeedback() = default;
    virtual void play(FeedbackPattern pattern) = 0;
};

// Persistent remote state (spec 11.5). Court MAC / channel / keys are owned
// by the firmware radio layer; this holds what the core logic needs.
struct RemoteSettings {
    bool paired = false;
    std::uint32_t remote_id = 0;
    CourtId court_id = 0;
    TeamId team{TeamId::A};
    // Sequences at or below this value may have been used before a reboot.
    std::uint32_t sequence_baseline = 0;
};

class ISettingsStore {
public:
    virtual ~ISettingsStore() = default;
    virtual std::optional<RemoteSettings> load() = 0;
    virtual bool save(const RemoteSettings& settings) = 0;
};

// Maps a terminal ACK status to the feedback pattern (spec 11.3 table).
FeedbackPattern feedback_for(protocol::AckStatus status);

// --- Core ---------------------------------------------------------------------

struct RemoteCoreConfig {
    // Debounce (spec 11.2 initial parameters; tunable).
    std::uint32_t stable_press_ms = 30;
    std::uint32_t stable_release_ms = 30;
    std::uint32_t retrigger_guard_ms = 700;
    // Retry policy (docs/RADIO_PROTOCOL.md).
    std::uint32_t ack_timeout_ms = 450;
    std::uint8_t max_attempts = 5;
    std::uint32_t backoff_ms[5] = {0, 80, 180, 350, 650};  // before attempt N+1
    // Persist the sequence baseline every N sequences (spec 11.5: avoid
    // identity reuse without wearing NVS on every press).
    std::uint32_t sequence_persist_chunk = 32;
    // Pairing (spec 11.1): long hold while unpaired enters advertise mode;
    // PAIR_REQUEST broadcast interval; give up after the timeout.
    std::uint32_t pairing_hold_ms = 5000;
    std::uint32_t pairing_advertise_interval_ms = 500;
    std::uint32_t pairing_timeout_ms = 60'000;
};

enum class RemoteState : std::uint8_t {
    PairingRequired,
    PairingAdvertise,
    Ready,
    PendingIntent,
};

class RemoteCore {
public:
    RemoteCore(RemoteCoreConfig config,
               const IClock& clock,
               IRadio& radio,
               IFeedback& feedback,
               ISettingsStore& store);

    // Loads persisted settings and advances the sequence past any possibly
    // used identity. boot_id must come from a hardware random source.
    // device_id is the hardware-derived logical remote id (e.g. from the
    // MAC); it is used when no persisted identity exists yet.
    void begin(std::uint32_t boot_id, std::uint32_t device_id = 0);

    // Raw button level (true = pressed); the core debounces internally.
    void set_button_level(bool pressed);

    // Feed every ACK received from the radio.
    void on_ack(const protocol::AckPacket& ack);

    // Feed PAIR_ASSIGN packets; ignored unless advertising and addressed to
    // this remote.
    void on_pair_assign(const protocol::PairAssignPacket& packet);

    // Drive timers (debounce, retries, timeouts, pairing). Call frequently.
    void poll();

    // Enter pairing-advertise mode (long hold does this automatically while
    // unpaired; firmware may also expose a service gesture).
    void enter_pairing_mode();

    // Pairing assignment (used by the pairing flow / provisioning).
    void apply_pairing(std::uint32_t remote_id, CourtId court_id, TeamId team);
    void clear_pairing();

    RemoteState state() const;
    const RemoteSettings& settings() const { return settings_; }
    std::uint32_t boot_id() const { return boot_id_; }
    std::uint32_t last_sequence() const { return sequence_; }
    void set_battery_mv(std::uint16_t mv) { battery_mv_ = mv; }

    struct Stats {
        std::uint32_t presses = 0;           // debounced accepted presses
        std::uint32_t presses_suppressed = 0;  // guard / in-flight suppressions
        std::uint32_t intents_sent = 0;      // first transmissions
        std::uint32_t retries = 0;
        std::uint32_t confirmed = 0;         // Accepted or DuplicateAccepted
        std::uint32_t rejected = 0;
        std::uint32_t failed = 0;            // attempts exhausted
    };
    const Stats& stats() const { return stats_; }

private:
    void on_debounced_press();
    void start_intent();
    void transmit();
    void persist_baseline_if_needed();

    RemoteCoreConfig config_;
    const IClock& clock_;
    IRadio& radio_;
    IFeedback& feedback_;
    ISettingsStore& store_;

    RemoteSettings settings_{};
    std::uint32_t boot_id_ = 0;
    std::uint32_t sequence_ = 0;
    std::uint16_t battery_mv_ = 0;

    // Debounce state.
    bool raw_level_ = false;
    bool stable_level_ = false;
    std::uint64_t level_since_ms_ = 0;
    std::uint64_t last_accepted_press_ms_ = 0;
    std::uint64_t press_started_ms_ = 0;

    // Pairing-advertise state.
    bool advertising_ = false;
    std::uint64_t advertise_until_ms_ = 0;
    std::uint64_t next_advertise_ms_ = 0;

    // In-flight intent (stop-and-wait: at most one).
    struct Pending {
        protocol::PointIntentPacket packet{};
        std::uint8_t attempts = 0;       // transmissions performed
        std::uint64_t deadline_ms = 0;   // next timeout or retransmit time
        bool awaiting_ack = false;       // false = waiting out the backoff
    };
    std::optional<Pending> pending_{};

    Stats stats_{};
};

}  // namespace padel::remote
