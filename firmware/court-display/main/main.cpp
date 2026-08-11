// Court display firmware for the Waveshare ESP32-S3-Touch-LCD-7B.
//
// Task architecture (spec 12.5 / 23.3):
//   - ESP-NOW receive callback -> bounded queue (never blocks, drops on
//     overflow and counts it as a visible fault)
//   - application task: owns CourtService + PairingService + journal;
//     drains the radio queue and the UI command queue, ticks time-driven
//     state, sends ACKs, publishes an immutable UiModel snapshot
//   - LVGL task: rendering + touch only; UI callbacks enqueue commands,
//     never touch the service. Flash writes never happen on this task.
//
// All logic is the same natively tested code that runs in court-sim; this
// file is transport + task glue.

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "board_7b.hpp"
#include "buzzer.hpp"
#include "lvgl.h"
#include "nvs.h"
#include "padel/application/club_controller.hpp"
#include "padel/application/court_service.hpp"
#include "padel/application/pairing.hpp"
#include "padel/application/roster.hpp"
#include "padel/application/roster_file.hpp"
#include "padel/common/battery.hpp"
#include "padel/common/idle_dim.hpp"
#include "padel/common/log.hpp"
#include "padel/domain/club_round.hpp"
#include "padel/persistence/journal.hpp"
#include "padel/persistence/stdio_file_backend.hpp"
#include "padel/ui/court_ui.hpp"
#include "padel/ui/model_builder.hpp"
#include "storage.hpp"

namespace {

using namespace padel;

const char* TAG = "court";

constexpr CourtId kCourtId = CONFIG_PADEL_COURT_ID;
constexpr std::uint32_t kConflictWindowMs = 250;
constexpr uint8_t kBroadcastMac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr gpio_num_t kButtonAGpio = static_cast<gpio_num_t>(CONFIG_PADEL_COURT_BUTTON_A_GPIO);
constexpr gpio_num_t kButtonBGpio = static_cast<gpio_num_t>(CONFIG_PADEL_COURT_BUTTON_B_GPIO);

// --- Clock ----------------------------------------------------------------

class EspClock : public application::IClock {
public:
    std::uint64_t now_ms() const override {
        return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
    }
};

// --- Radio ------------------------------------------------------------------

struct RxFrame {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    int len;
    uint8_t data[48];
};

QueueHandle_t s_rx_queue = nullptr;
volatile uint32_t s_rx_overflows = 0;

void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len <= 0 || len > static_cast<int>(sizeof(RxFrame::data))) {
        return;
    }
    RxFrame frame{};
    memcpy(frame.mac, info->src_addr, ESP_NOW_ETH_ALEN);
    frame.len = len;
    memcpy(frame.data, data, static_cast<size_t>(len));
    if (xQueueSend(s_rx_queue, &frame, 0) != pdTRUE) {
        // Queue overflow is a visible fault (spec 12.5); the app task
        // surfaces the counter on the diagnostics screen.
        s_rx_overflows = s_rx_overflows + 1;
    }
}

void ensure_peer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) {
        return;
    }
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    peer.channel = CONFIG_PADEL_COURT_WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;  // encrypted peers land with key provisioning (M5)
    esp_now_add_peer(&peer);
}

void init_radio() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(
        esp_wifi_set_channel(CONFIG_PADEL_COURT_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    s_rx_queue = xQueueCreate(32, sizeof(RxFrame));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ensure_peer(kBroadcastMac);
}

// --- Wired buttons (spec 15) ------------------------------------------------

void init_gpio() {
    gpio_config_t in{};
    in.pin_bit_mask = (1ULL << kButtonAGpio) | (1ULL << kButtonBGpio);
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&in));
}

// Press debounce for the arcade buttons; one event per press. Matches
// RemoteCoreConfig::stable_press_ms so a shirt brushing the post does not
// score on either input path.
constexpr std::uint64_t kWiredButtonPressMs = 150;

class WiredButton {
public:
    explicit WiredButton(gpio_num_t pin) : pin_(pin) {}

    bool pressed_edge(std::uint64_t now_ms) {
        const bool level = gpio_get_level(pin_) == 0;  // active low
        if (level != raw_) {
            raw_ = level;
            edge_at_ms_ = now_ms;
        }
        if (raw_ != stable_ && now_ms - edge_at_ms_ >= kWiredButtonPressMs) {
            stable_ = raw_;
            if (stable_) {
                ++presses_;
                return true;
            }
        }
        return false;
    }

    // Debounced level and press count for the diagnostics screen, so wiring
    // can be checked without scoring a point.
    bool down() const { return stable_; }
    std::uint32_t presses() const { return presses_; }

private:
    gpio_num_t pin_;
    bool raw_ = false;
    bool stable_ = false;
    std::uint64_t edge_at_ms_ = 0;
    std::uint32_t presses_ = 0;
};

std::string wired_button_label(char team, int gpio) {
    return std::string("Wired button ") + team + " (GPIO" + std::to_string(gpio) + ")";
}

std::string wired_button_value(const WiredButton& button) {
    return std::string(button.down() ? "DOWN" : "up") + ", " +
           std::to_string(button.presses()) + " presses";
}

// Games played in the set under way. A finished game shows up here as a step
// up, which is how the buzzer hears one: the reducer stores no GameWon event
// (ADR-0003).
std::uint8_t games_in_current_set(const domain::MatchState& state) {
    return static_cast<std::uint8_t>(state.current_set.games_a + state.current_set.games_b);
}

// --- UI command queue (LVGL task -> app task) --------------------------------

struct AppCommand {
    enum class Type {
        AwardPoint,
        UndoConfirmed,
        TogglePause,
        ResolveConflict,
        StartMatch,
        ResetConfirmed,
        NewMatch,
        SummaryContinue,
        ShowScreen,
        BeginPairing,
        CancelPairing,
        ConfirmPairing,
        UnpairRemote,
        RecoveryChoice,
        TestBeep,
        CreatePlayer,
        StartClubRound,
        ClubNextSet,
        ClubNewRound,
        ClubDone,
        SetBrightness,
    };
    Type type{};
    TeamId team{TeamId::A};
    std::optional<TeamId> winner{};
    ui::MatchSettings settings{};
    ui::Screen screen{ui::Screen::Setup};
    bool resume = false;
    std::string player_name{};
    std::array<ui::ClubPlayer, 4> club_players{};
    std::uint8_t brightness = 100;
};

std::mutex s_command_mutex;
std::vector<AppCommand> s_commands;

void push_command(AppCommand command) {
    std::lock_guard<std::mutex> lock(s_command_mutex);
    s_commands.push_back(std::move(command));
}

// --- Shared UI model (app task -> LVGL task) ---------------------------------

std::mutex s_model_mutex;
ui::UiModel s_shared_model;
bool s_model_dirty = false;

void publish_model(const ui::UiModel& model) {
    std::lock_guard<std::mutex> lock(s_model_mutex);
    s_shared_model = model;
    s_model_dirty = true;
}

constexpr char kBrightnessNvsKey[] = "bright";

// --- Idle backlight (ADR-0020) ----------------------------------------------

constexpr power::IdlePolicy kIdlePolicy{
    static_cast<std::uint32_t>(CONFIG_PADEL_COURT_IDLE_DIM_MIN) * 60u * 1000u,
    static_cast<std::uint32_t>(CONFIG_PADEL_COURT_IDLE_OFF_MIN) * 60u * 1000u,
    static_cast<std::uint8_t>(CONFIG_PADEL_COURT_IDLE_DIM_PERCENT),
};

// Input clock shared app task -> LVGL task. Touch is already covered by LVGL's
// own inactivity timer, so this only carries the paths LVGL cannot see:
// remote points, pair requests, wired buttons and queued UI commands.
std::atomic<std::uint64_t> s_last_input_ms{0};
// Organizer brightness (slider / NVS); the awake level the idle stages ride on.
std::atomic<std::uint8_t> s_user_brightness{100};
std::atomic<std::uint8_t> s_display_stage{
    static_cast<std::uint8_t>(power::DisplayStage::Awake)};

void note_input() {
    s_last_input_ms.store(static_cast<std::uint64_t>(esp_timer_get_time() / 1000),
                          std::memory_order_relaxed);
}

// Diagnostics text: current stage plus the configured idle windows.
std::string display_state_label() {
    const auto stage =
        static_cast<power::DisplayStage>(s_display_stage.load(std::memory_order_relaxed));
    std::string label = power::stage_label(stage);
    if (stage == power::DisplayStage::Dimmed) {
        label += " " + std::to_string(kIdlePolicy.dim_percent) + "%";
    }
    if (kIdlePolicy.dim_percent == 0 || kIdlePolicy.dim_after_ms == 0) {
        return label + " (idle dim disabled)";
    }
    label += " (dim " + std::to_string(CONFIG_PADEL_COURT_IDLE_DIM_MIN) + "m";
    if (kIdlePolicy.off_after_ms != 0) {
        label += ", off " + std::to_string(CONFIG_PADEL_COURT_IDLE_OFF_MIN) + "m";
    }
    return label + ")";
}

std::mutex s_battery_mutex;
std::optional<std::uint16_t> s_battery_mv;

std::optional<std::uint16_t> latest_battery_mv() {
    std::lock_guard<std::mutex> lock(s_battery_mutex);
    return s_battery_mv;
}

void publish_battery_mv(std::optional<std::uint16_t> mv) {
    std::lock_guard<std::mutex> lock(s_battery_mutex);
    s_battery_mv = mv;
}

std::uint8_t load_brightness_nvs() {
    nvs_handle_t handle = 0;
    std::uint8_t value = 100;
    if (nvs_open("padel_court", NVS_READONLY, &handle) != ESP_OK) {
        return value;
    }
    nvs_get_u8(handle, kBrightnessNvsKey, &value);
    nvs_close(handle);
    if (value < 10) {
        value = 10;
    }
    if (value > 100) {
        value = 100;
    }
    return value;
}

void save_brightness_nvs(std::uint8_t percent) {
    nvs_handle_t handle = 0;
    if (nvs_open("padel_court", NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (nvs_set_u8(handle, kBrightnessNvsKey, percent) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

// --- Application ------------------------------------------------------------

struct CourtApp {
    EspClock clock{};
    ui::UiModel model{};
    ui::MatchSettings settings{};

    storage::NvsSettings court_settings{};
    std::unique_ptr<persistence::StdioFileBackend> file;
    std::unique_ptr<persistence::JournalEventStore> store;
    std::unique_ptr<application::CourtService> service;
    std::unique_ptr<application::PairingService> pairing;

    // Club round: roster + results on LittleFS, controller over the match
    // lifecycle. An in-flight round is RAM-only (a mini-set is minutes long).
    std::unique_ptr<application::FileRosterStore> roster_store;
    std::unique_ptr<application::PlayerRoster> roster;
    std::unique_ptr<application::FileResultsLog> results_log;
    std::unique_ptr<application::ClubController> club;
    bool club_active = false;
    std::string club_hint;
    std::vector<ui::ClubPlayer> club_suggested_a;
    std::vector<ui::ClubPlayer> club_suggested_b;
    std::uint32_t club_suggestion_seq = 0;

    WiredButton button_a{kButtonAGpio};
    WiredButton button_b{kButtonBGpio};

    // remote_id -> last known MAC, for unicast ACKs.
    std::unordered_map<std::uint32_t, std::array<uint8_t, ESP_NOW_ETH_ALEN>> remote_macs;

    std::uint64_t boot_ms = 0;
    std::uint64_t match_started_ms = 0;
    std::optional<TeamId> flash_team{};
    std::uint64_t flash_until_ms = 0;
    std::uint8_t prev_points_a = 0;
    std::uint8_t prev_points_b = 0;
    std::uint8_t prev_completed_sets = 0;
    std::uint8_t prev_games_in_set = 0;
    std::uint32_t acked_remote_undos = 0;
    domain::MatchLifecycle prev_lifecycle = domain::MatchLifecycle::NotStarted;
    bool storage_degraded = false;

    std::uint8_t brightness_percent = 100;
    std::optional<std::uint16_t> battery_mv{};

    void create_pairing_service() {
        pairing = std::make_unique<application::PairingService>(
            application::PairingService::Config{kCourtId, CONFIG_PADEL_COURT_WIFI_CHANNEL,
                                                30'000},
            *service, court_settings, clock);
        pairing->load_assignments();
    }

    // Boot sequence per spec 12.2: mount -> recover journal -> rebuild
    // service -> restore allow-list -> Recovery or Setup screen.
    void boot(bool storage_ok) {
        storage_degraded = !storage_ok;

        // Roster seeds itself (in RAM only when the mount failed).
        roster_store = std::make_unique<application::FileRosterStore>("/littlefs/roster.txt");
        roster = std::make_unique<application::PlayerRoster>(*roster_store);
        results_log =
            std::make_unique<application::FileResultsLog>("/littlefs/club_results.csv");
        club = std::make_unique<application::ClubController>(*results_log, clock);

        std::vector<application::CommittedEvent> recovered_events;
        persistence::TailStatus tail = persistence::TailStatus::Clean;
        std::size_t valid_bytes = 0;
        if (storage_ok) {
            file = std::make_unique<persistence::StdioFileBackend>(storage::journal_path());
            const persistence::RecoveryResult recovered = persistence::recover(file->read_all());
            file->truncate(recovered.valid_bytes);
            logging::emit(logging::Level::Info, "storage.recovered", "events=%zu tail=%s",
                          recovered.events.size(),
                          recovered.tail == persistence::TailStatus::Clean ? "clean"
                                                                           : "damaged");
            recovered_events = recovered.events;
            tail = recovered.tail;
            valid_bytes = recovered.valid_bytes;
            store = std::make_unique<persistence::JournalEventStore>(*file, valid_bytes);
        } else {
            // Degraded: keep scoring on a RAM journal; surface the fault.
            logging::emit(logging::Level::Error, "storage.mount_failed", "fs=littlefs");
            file = std::make_unique<persistence::StdioFileBackend>("/dev/null");
            store = std::make_unique<persistence::JournalEventStore>(*file, 0);
        }

        const std::size_t recovered_count = recovered_events.size();
        service = std::make_unique<application::CourtService>(
            application::CourtServiceConfig{kCourtId, kConflictWindowMs},
            ui::preset_config(settings.preset_index), std::move(recovered_events), *store,
            clock);
        create_pairing_service();
        boot_ms = clock.now_ms();
        prev_lifecycle = service->state().lifecycle;
        prev_points_a = service->state().current_game.raw_points_a;
        prev_points_b = service->state().current_game.raw_points_b;
        // A restored journal must not sound the milestones it already played.
        prev_completed_sets = service->state().completed_set_count;
        prev_games_in_set = games_in_current_set(service->state());

        const auto lifecycle = service->state().lifecycle;
        if (lifecycle == domain::MatchLifecycle::Active ||
            lifecycle == domain::MatchLifecycle::Paused) {
            model.screen = ui::Screen::Recovery;
            model.recovery.message = "A match was in progress when power was lost.";
            char detail[128];
            std::snprintf(detail, sizeof(detail), "Journal: %zu events recovered, tail %s.",
                          recovered_count,
                          tail == persistence::TailStatus::Clean ? "clean"
                                                                 : "damaged (truncated)");
            model.recovery.detail = detail;
            model.recovery.corrupt_tail = tail != persistence::TailStatus::Clean;
            ESP_LOGI(TAG, "boot: resumable match found (%zu events)", recovered_count);
        } else {
            model.screen = ui::Screen::Setup;
            ESP_LOGI(TAG, "boot: no resumable match, showing setup");
        }
    }

    void fresh_service() {
        pairing.reset();
        service.reset();
        store.reset();
        file.reset();
        storage::archive_journal();
        file = std::make_unique<persistence::StdioFileBackend>(storage::journal_path());
        store = std::make_unique<persistence::JournalEventStore>(*file, 0);
        service = std::make_unique<application::CourtService>(
            application::CourtServiceConfig{kCourtId, kConflictWindowMs},
            ui::preset_config(settings.preset_index), *store, clock);
        create_pairing_service();
    }

    // --- Club round ----------------------------------------------------------
    // Leaves an in-flight club round without inventing a new forbidden pair
    // (finish_round only records Top 2 when the round is Complete).
    void abandon_club_round() {
        if (club->round_active()) {
            club->finish_round();
        }
        club_active = false;
        club_hint.clear();
    }

    // Starts the mini-set the controller is waiting on as a normal journaled
    // match with the pairing as team names (same code path as court-sim).
    void begin_club_set() {
        const auto teams = club->current_set_teams();
        settings.team_a_name = teams.team_a;
        settings.team_b_name = teams.team_b;
        settings.players_a.clear();
        settings.players_b.clear();
        settings.preset_index = ui::kClubRoundPreset;

        if (service->state().lifecycle != domain::MatchLifecycle::NotStarted ||
            file->size() > 0) {
            fresh_service();
        }
        if (service->start_match(settings.first_server)) {
            match_started_ms = clock.now_ms();
            model.screen = ui::Screen::Live;
            ESP_LOGI(TAG, "club set %d started", club->set_number());
        }
    }

    // The summary reads back the mini-set that just finished, so it names the
    // set the club round has already moved past.
    std::string summary_title() const {
        if (!club_active) {
            return {};
        }
        return club->stage() == domain::ClubStage::Complete ? "CLUB SET 2 COMPLETE"
                                                            : "CLUB SET 1 COMPLETE";
    }

    std::string summary_continue_label() const {
        if (!club_active) {
            return "CONTINUE";
        }
        return club->stage() == domain::ClubStage::Complete ? "SEE STANDINGS" : "MIX IT UP";
    }

    // CONTINUE on the summary: a club round carries on to the mix or the
    // standings, an ordinary match ends on the complete screen.
    void advance_past_summary() {
        if (!club_active) {
            model.screen = ui::Screen::MatchComplete;
            return;
        }
        model.screen = club->stage() == domain::ClubStage::Set2 ? ui::Screen::ClubMix
                                                                : ui::Screen::ClubStandings;
    }

    // --- Radio inbound -------------------------------------------------------
    void handle_frame(const RxFrame& frame) {
        const auto type =
            protocol::peek_message_type(frame.data, static_cast<size_t>(frame.len));
        if (!type) {
            return;
        }
        if (type.value() == protocol::MessageType::PointIntent) {
            const auto intent =
                protocol::parse_point_intent(frame.data, static_cast<size_t>(frame.len));
            if (intent) {
                note_input();
                remember_mac(intent.value().identity.remote_id, frame.mac);
                service->handle_point_intent(intent.value());
            }
        } else if (type.value() == protocol::MessageType::PairRequest) {
            const auto request =
                protocol::parse_pair_request(frame.data, static_cast<size_t>(frame.len));
            if (request) {
                note_input();
                remember_mac(request.value().remote_id, frame.mac);
                pairing->handle_pair_request(request.value());
            }
        }
    }

    void remember_mac(std::uint32_t remote_id, const uint8_t* mac) {
        std::array<uint8_t, ESP_NOW_ETH_ALEN> stored{};
        memcpy(stored.data(), mac, ESP_NOW_ETH_ALEN);
        remote_macs[remote_id] = stored;
        ensure_peer(mac);
    }

    void send_acks() {
        for (const protocol::AckPacket& ack : service->drain_acks()) {
            const auto bytes = protocol::serialize(ack);
            const auto it = remote_macs.find(ack.identity.remote_id);
            const uint8_t* target = it != remote_macs.end() ? it->second.data() : kBroadcastMac;
            esp_now_send(target, bytes.data(), bytes.size());
            if (ack.status == protocol::AckStatus::Accepted) {
                buzzer::play(sound::Cue::PointScored);
            }
        }
        // A remote hold reverses a point with nobody touching the court, so it
        // gets the falling cue instead of sounding like a score. Playing it
        // here replaces the point cue that the same batch may have started.
        const std::uint32_t undos = service->counters().remote_undos;
        const bool grew = undos > acked_remote_undos;
        acked_remote_undos = undos;  // a rebuilt service restarts at zero
        if (grew) {
            buzzer::play(sound::Cue::RemoteUndo);
        }
    }

    // --- UI commands ---------------------------------------------------------
    void handle_command(const AppCommand& command) {
        using Type = AppCommand::Type;
        note_input();
        switch (command.type) {
            case Type::AwardPoint:
                service->award_point_local(command.team, InputSource::TouchscreenAdmin);
                break;
            case Type::UndoConfirmed:
                service->undo_last_scoring_action();
                break;
            case Type::TogglePause:
                if (service->state().lifecycle == domain::MatchLifecycle::Paused) {
                    service->resume_match();
                } else {
                    service->pause_match();
                }
                break;
            case Type::ResolveConflict:
                service->resolve_conflict(command.winner);
                break;
            case Type::StartMatch:
                abandon_club_round();
                settings = command.settings;
                if (service->state().lifecycle != domain::MatchLifecycle::NotStarted ||
                    file->size() > 0) {
                    fresh_service();
                }
                if (service->start_match(settings.first_server)) {
                    match_started_ms = clock.now_ms();
                    model.screen = ui::Screen::Live;
                    ESP_LOGI(TAG, "match started");
                }
                break;
            case Type::ResetConfirmed:
                abandon_club_round();
                service->reset_match();
                model.screen = ui::Screen::Setup;
                break;
            case Type::NewMatch:
                abandon_club_round();
                model.screen = ui::Screen::Setup;
                break;
            case Type::SummaryContinue:
                advance_past_summary();
                break;
            case Type::ShowScreen:
                model.screen = command.screen;
                break;
            case Type::BeginPairing:
                pairing->begin(command.team);
                model.screen = ui::Screen::Pairing;
                break;
            case Type::CancelPairing:
                pairing->cancel();
                model.screen = ui::Screen::Setup;
                break;
            case Type::ConfirmPairing:
                if (const auto assign = pairing->confirm()) {
                    const auto bytes = protocol::serialize(*assign);
                    esp_now_send(kBroadcastMac, bytes.data(), bytes.size());
                    buzzer::play(sound::Cue::PairingConfirmed);
                }
                model.screen = ui::Screen::Setup;
                break;
            case Type::UnpairRemote:
                // The allow-list permits several remotes per team, so drain
                // the team rather than dropping only the one the UI showed.
                while (const auto info = service->remote_info(command.team)) {
                    pairing->unassign(info->remote_id);
                }
                break;
            case Type::RecoveryChoice:
                if (command.resume) {
                    model.screen = ui::Screen::Live;
                    match_started_ms = clock.now_ms();
                } else {
                    abandon_club_round();
                    fresh_service();
                    model.screen = ui::Screen::Setup;
                }
                break;
            case Type::TestBeep:
                buzzer::play(sound::Cue::SelfTest);
                break;
            case Type::SetBrightness:
                // Only the organizer's own level is persisted; idle stages never
                // reach NVS, so a reboot comes up at the chosen brightness.
                brightness_percent = command.brightness;
                save_brightness_nvs(brightness_percent);
                break;
            case Type::CreatePlayer:
                roster->add_player(command.player_name);
                break;
            case Type::StartClubRound: {
                // Setup always means start fresh; orphaned rounds (RESET / NEW
                // MATCH without finish) must not block the next start.
                if (club->round_active()) {
                    abandon_club_round();
                }
                settings = command.settings;
                std::array<application::Player, 4> players{};
                for (std::size_t i = 0; i < 4; ++i) {
                    players[i] = application::Player{command.club_players[i].id,
                                                     command.club_players[i].name,
                                                     command.club_players[i].guest};
                }
                const auto error = club->start_round(players, esp_random(),
                                                     ui::crown_pairs(command.club_players));
                if (error == application::ClubController::StartError::ForbiddenPair) {
                    club_hint =
                        "Crowned pairs can't be teammates - put them on opposite sides";
                    break;
                }
                if (error == application::ClubController::StartError::DuplicatePlayer) {
                    club_hint = "Same player picked twice - split them up";
                    break;
                }
                if (error.has_value()) {
                    club_hint = "A club round is already in progress";
                    break;
                }
                club_hint.clear();
                club_active = true;
                begin_club_set();
                break;
            }
            case Type::ClubNextSet:
                if (club_active && club->stage() == domain::ClubStage::Set2) {
                    begin_club_set();
                }
                break;
            case Type::ClubNewRound:
                // Capture Top 2 / Bottom 2 before finish_round clears the round.
                if (ui::suggest_next_round_picks(*club, club_suggested_a, club_suggested_b)) {
                    ++club_suggestion_seq;
                }
                club->finish_round();
                club_active = false;
                club_hint.clear();
                model.screen = ui::Screen::Setup;
                break;
            case Type::ClubDone:
                club->finish_round();
                club_active = false;
                club_hint.clear();
                model.screen = ui::Screen::Setup;
                break;
        }
    }

    // --- Frame ----------------------------------------------------------------
    void build_diagnostics(std::uint64_t now) {
        auto& rows = model.diagnostics.rows;
        rows.clear();
        const auto& counters = service->counters();
        const auto& dedup = service->deduplicator().counters();
        rows.push_back({"Firmware", "court-display esp32s3"});
        rows.push_back({"Board profile", "Waveshare 7B 1024x600"});
        rows.push_back({"Court id", std::to_string(kCourtId)});
        rows.push_back({"Radio channel", std::to_string(CONFIG_PADEL_COURT_WIFI_CHANNEL)});
        const auto soc = battery_mv ? padel::battery::mv_to_percent(*battery_mv) : std::nullopt;
        if (soc) {
            rows.push_back({"Battery", std::to_string(*soc) + "%"});
        } else {
            rows.push_back({"Battery", "unknown / no cell"});
        }
        if (battery_mv) {
            char volt[16];
            std::snprintf(volt, sizeof(volt), "%u.%02u V", *battery_mv / 1000u,
                          (*battery_mv % 1000u) / 10u);
            rows.push_back({"Battery voltage", volt});
        } else {
            rows.push_back({"Battery voltage", "n/a"});
        }
        rows.push_back(
            {"Est. runtime", padel::battery::format_runtime_estimate(soc)});
        rows.push_back({"Brightness", std::to_string(brightness_percent) + "%"});
        rows.push_back({"Display", display_state_label()});
        rows.push_back({"Buzzer (GPIO" + std::to_string(CONFIG_PADEL_COURT_BUZZER_GPIO) + ")",
                        buzzer::kind()});
        rows.push_back({wired_button_label('A', CONFIG_PADEL_COURT_BUTTON_A_GPIO),
                        wired_button_value(button_a)});
        rows.push_back({wired_button_label('B', CONFIG_PADEL_COURT_BUTTON_B_GPIO),
                        wired_button_value(button_b)});
        rows.push_back({"Accepted", std::to_string(counters.accepted)});
        rows.push_back({"Duplicates", std::to_string(counters.duplicates)});
        rows.push_back({"Rejected", std::to_string(counters.rejected)});
        rows.push_back({"Conflicts", std::to_string(counters.conflicts)});
        rows.push_back({"Remote undos", std::to_string(counters.remote_undos)});
        rows.push_back({"Storage failures", std::to_string(counters.storage_failures)});
        rows.push_back({"Dedup accepted/dup/stale",
                        std::to_string(dedup.accepted) + "/" + std::to_string(dedup.duplicates) +
                            "/" + std::to_string(dedup.stale)});
        rows.push_back({"RX queue overflows", std::to_string(s_rx_overflows)});
        rows.push_back({"Journal size", std::to_string(file->size()) + " B"});
        rows.push_back({"State revision", std::to_string(service->state().revision)});
        rows.push_back({"Log records", std::to_string(padel::logging::total_emitted())});
        rows.push_back({"Free heap", std::to_string(esp_get_free_heap_size()) + " B"});
        rows.push_back({"Uptime", std::to_string((now - boot_ms) / 1000) + " s"});
        model.diagnostics.recent_log_lines = padel::logging::recent_lines(10);
    }

    void frame(std::uint64_t now) {
        service->tick();
        pairing->tick();
        send_acks();

        // ADC is sampled on the LVGL task (shared I2C with touch).
        battery_mv = latest_battery_mv();

        // Wired backup buttons participate in the conflict guard (spec 15).
        if (button_a.pressed_edge(now)) {
            note_input();
            service->award_point_local(TeamId::A, InputSource::PhysicalBackupButton);
        }
        if (button_b.pressed_edge(now)) {
            note_input();
            service->award_point_local(TeamId::B, InputSource::PhysicalBackupButton);
        }

        // Point flash edge detection (works for every input path).
        const auto& state = service->state();
        const std::uint8_t points_a =
            state.in_tiebreak ? state.tiebreak_points_a : state.current_game.raw_points_a;
        const std::uint8_t points_b =
            state.in_tiebreak ? state.tiebreak_points_b : state.current_game.raw_points_b;
        if (points_a > prev_points_a) {
            flash_team = TeamId::A;
            flash_until_ms = now + 250;
        }
        if (points_b > prev_points_b) {
            flash_team = TeamId::B;
            flash_until_ms = now + 250;
        }
        prev_points_a = points_a;
        prev_points_b = points_b;

        // Milestone cues, loudest thing first: a point that ends the match says
        // only that, and one that ends a set says only that, never both.
        const std::uint8_t completed_sets = state.completed_set_count;
        const std::uint8_t games_in_set = games_in_current_set(state);
        const bool match_completed_edge = state.lifecycle == domain::MatchLifecycle::Completed &&
                                          prev_lifecycle != domain::MatchLifecycle::Completed;
        // An undo that reopens a set puts its games back, so both counters can
        // climb with nothing having been won. Only a forward step is a milestone.
        const bool rewound = completed_sets < prev_completed_sets ||
                             (prev_lifecycle == domain::MatchLifecycle::Completed &&
                              state.lifecycle != domain::MatchLifecycle::Completed);

        if (match_completed_edge && model.screen == ui::Screen::Live) {
            // The finished mini-set feeds the club round; CONTINUE on the
            // summary then moves on to the mix or the standings.
            if (club_active) {
                club->on_set_complete(state);
            }
            model.screen = ui::Screen::MatchSummary;
            buzzer::play(sound::Cue::MatchComplete);
        } else if (prev_lifecycle == domain::MatchLifecycle::Completed &&
                   state.lifecycle != domain::MatchLifecycle::Completed &&
                   ui::is_post_match_screen(model.screen)) {
            // An undo took the winning point back: walk the whole flow back
            // to the live screen, club round included.
            if (club_active) {
                club->undo_last_set();
            }
            model.screen = ui::Screen::Live;
            ESP_LOGI(TAG, "undo reopened the match");
        } else if (!match_completed_edge && !rewound) {
            if (completed_sets > prev_completed_sets) {
                buzzer::play(sound::Cue::SetComplete);
            } else if (games_in_set > prev_games_in_set) {
                buzzer::play(sound::Cue::GameComplete);
            }
        }
        prev_lifecycle = state.lifecycle;
        prev_completed_sets = completed_sets;
        prev_games_in_set = games_in_set;

        model.settings = settings;
        model.live = ui::build_live_model(*service, settings, now);
        model.live.brightness_percent = brightness_percent;
        if (battery_mv) {
            model.live.battery_percent = padel::battery::mv_to_percent(*battery_mv);
        } else {
            model.live.battery_percent = std::nullopt;
        }
        if (now < flash_until_ms) {
            model.live.point_flash = flash_team;
        }
        if (storage_degraded) {
            model.live.storage_fault = true;
        }
        const std::uint64_t match_duration_ms =
            match_started_ms > 0 ? now - match_started_ms : 0;
        model.complete = ui::build_complete_model(*service, settings, match_duration_ms);
        model.summary = ui::build_summary_model(*service, settings, match_duration_ms,
                                                summary_title(), summary_continue_label());
        model.club = ui::build_club_model(*roster, *club, club_hint);
        model.club.suggested_a = club_suggested_a;
        model.club.suggested_b = club_suggested_b;
        model.club.suggestion_seq = club_suggestion_seq;

        if (model.screen == ui::Screen::Pairing) {
            if (!pairing->active()) {
                model.screen = ui::Screen::Setup;
            } else {
                const TeamId team = *pairing->team();
                model.pairing.team_label =
                    std::string("Pairing: TEAM ") + (team == TeamId::A ? "A" : "B");
                model.pairing.instruction =
                    "Hold the remote button for 5 seconds to enter pairing mode.";
                const auto& candidate = pairing->candidate();
                model.pairing.candidate_label =
                    candidate ? "Remote " + candidate->short_id + " requests pairing" : "";
                model.pairing.awaiting_confirm = candidate.has_value();
                model.pairing.seconds_left = static_cast<int>(pairing->seconds_left());
            }
        }
        build_diagnostics(now);
        publish_model(model);
    }
};

// --- UI callbacks (run on the LVGL task; only enqueue) ------------------------

ui::UiCallbacks make_callbacks() {
    ui::UiCallbacks cb{};
    cb.award_point = [](TeamId team) {
        push_command({.type = AppCommand::Type::AwardPoint, .team = team});
    };
    cb.undo_confirmed = []() { push_command({.type = AppCommand::Type::UndoConfirmed}); };
    cb.toggle_pause = []() { push_command({.type = AppCommand::Type::TogglePause}); };
    cb.resolve_conflict = [](std::optional<TeamId> winner) {
        AppCommand command{.type = AppCommand::Type::ResolveConflict};
        command.winner = winner;
        push_command(std::move(command));
    };
    cb.start_match = [](const ui::MatchSettings& settings) {
        AppCommand command{.type = AppCommand::Type::StartMatch};
        command.settings = settings;
        push_command(std::move(command));
    };
    cb.reset_confirmed = []() { push_command({.type = AppCommand::Type::ResetConfirmed}); };
    cb.new_match = []() { push_command({.type = AppCommand::Type::NewMatch}); };
    cb.summary_continue = []() { push_command({.type = AppCommand::Type::SummaryContinue}); };
    cb.show_screen = [](ui::Screen screen) {
        push_command({.type = AppCommand::Type::ShowScreen, .screen = screen});
    };
    cb.begin_pairing = [](TeamId team) {
        push_command({.type = AppCommand::Type::BeginPairing, .team = team});
    };
    cb.cancel_pairing = []() { push_command({.type = AppCommand::Type::CancelPairing}); };
    cb.confirm_pairing = []() { push_command({.type = AppCommand::Type::ConfirmPairing}); };
    cb.unpair_remote = [](TeamId team) {
        push_command({.type = AppCommand::Type::UnpairRemote, .team = team});
    };
    cb.recovery_choice = [](bool resume) {
        push_command({.type = AppCommand::Type::RecoveryChoice, .resume = resume});
    };
    cb.test_beep = []() { push_command({.type = AppCommand::Type::TestBeep}); };
    // The idle manager owns the PWM write (same LVGL task, which also owns the
    // I2C bus shared with touch); persist via the app task so NVS writes stay
    // off the UI thread.
    cb.set_brightness = [](std::uint8_t percent) {
        s_user_brightness.store(percent, std::memory_order_relaxed);
        AppCommand command{.type = AppCommand::Type::SetBrightness};
        command.brightness = percent;
        push_command(std::move(command));
    };
    cb.create_player = [](const std::string& name) {
        AppCommand command{.type = AppCommand::Type::CreatePlayer};
        command.player_name = name;
        push_command(std::move(command));
    };
    cb.start_club_round = [](const std::array<ui::ClubPlayer, 4>& players,
                             const ui::MatchSettings& settings) {
        AppCommand command{.type = AppCommand::Type::StartClubRound};
        command.club_players = players;
        command.settings = settings;
        push_command(std::move(command));
    };
    cb.club_next_set = []() { push_command({.type = AppCommand::Type::ClubNextSet}); };
    cb.club_new_round = []() { push_command({.type = AppCommand::Type::ClubNewRound}); };
    cb.club_done = []() { push_command({.type = AppCommand::Type::ClubDone}); };
    return cb;
}

// --- Tasks --------------------------------------------------------------------

// LVGL objects are only ever touched from this task.
void lvgl_task(void* /*arg*/) {
    if (!board::init_display()) {
        ESP_LOGE(TAG, "display init failed; LVGL task idle");
        vTaskDelete(nullptr);
        return;
    }
    const std::uint8_t restored = load_brightness_nvs();
    s_user_brightness.store(restored, std::memory_order_relaxed);
    board::set_brightness(restored);
    {
        AppCommand command{.type = AppCommand::Type::SetBrightness};
        command.brightness = restored;
        push_command(std::move(command));
    }
    ui::init_theme();

    static ui::CourtUi court_ui;
    court_ui.create(make_callbacks());

    ui::UiModel local_model;
    std::uint64_t last_render_ms = 0;
    std::uint64_t last_battery_sample_ms = 0;
    std::uint8_t applied_brightness = restored;
    std::uint64_t touch_wake_ms = 0;
    while (true) {
        lv_timer_handler();

        const std::uint64_t now = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
        if (last_battery_sample_ms == 0 || now - last_battery_sample_ms >= 5000) {
            publish_battery_mv(board::read_battery_mv());
            last_battery_sample_ms = now;
        }

        // Idle backlight stages (ADR-0020). Touch comes from LVGL's own
        // inactivity clock, except for the gated tap that ends a dim: that one
        // never reaches LVGL, so it reports through the board wake latch.
        if (board::consume_touch_wake()) {
            touch_wake_ms = now;
        }
        const std::uint64_t last_input =
            std::max(s_last_input_ms.load(std::memory_order_relaxed), touch_wake_ms);
        const std::uint32_t idle_ms =
            std::min(static_cast<std::uint32_t>(now - last_input),
                     static_cast<std::uint32_t>(lv_disp_get_inactive_time(nullptr)));
        const auto stage = power::stage_for_idle(idle_ms, kIdlePolicy);
        const std::uint8_t target = power::applied_percent(
            stage, s_user_brightness.load(std::memory_order_relaxed), kIdlePolicy);
        if (target != applied_brightness) {
            board::set_brightness(target);
            applied_brightness = target;
            ESP_LOGI(TAG, "display %s: backlight %u%%", power::stage_label(stage), target);
        }
        board::set_touch_gate(stage != power::DisplayStage::Awake);
        s_display_stage.store(static_cast<std::uint8_t>(stage), std::memory_order_relaxed);

        bool render = false;
        {
            std::lock_guard<std::mutex> lock(s_model_mutex);
            // Coarse re-render keeps countdowns/liveness fresh even without
            // model changes.
            if (s_model_dirty && now - last_render_ms >= 50) {
                local_model = s_shared_model;
                s_model_dirty = false;
                render = true;
            }
        }
        if (render) {
            court_ui.render(local_model);
            last_render_ms = now;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_task(void* arg) {
    auto* app = static_cast<CourtApp*>(arg);
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));

    const bool storage_ok = storage::mount();
    if (!app->court_settings.open()) {
        ESP_LOGE(TAG, "NVS settings open failed");
    }
    app->boot(storage_ok);

    RxFrame frame;
    std::vector<AppCommand> commands;
    while (true) {
        esp_task_wdt_reset();

        while (xQueueReceive(s_rx_queue, &frame, 0) == pdTRUE) {
            app->handle_frame(frame);
        }

        commands.clear();
        {
            std::lock_guard<std::mutex> lock(s_command_mutex);
            commands.swap(s_commands);
        }
        for (const AppCommand& command : commands) {
            app->handle_command(command);
        }

        app->frame(app->clock.now_ms());
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace

namespace {

std::uint64_t log_clock() {
    return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
}

void log_sink(const logging::Entry& entry) {
    // Structured events go to serial alongside ESP_LOG output (spec 16).
    printf("[evt %8llu] %s %s\n", static_cast<unsigned long long>(entry.t_ms), entry.event,
           entry.detail);
}

}  // namespace

extern "C" void app_main(void) {
    logging::set_clock(log_clock);
    logging::set_sink(log_sink);

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    init_radio();
    init_gpio();
    buzzer::init();

    static CourtApp app;

    // LVGL on core 1 (rendering-heavy), application on core 0 alongside the
    // Wi-Fi stack so radio->service hand-off stays on one core.
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, nullptr, 4, nullptr, 1);
    xTaskCreatePinnedToCore(app_task, "app", 12288, &app, 5, nullptr, 0);
}
