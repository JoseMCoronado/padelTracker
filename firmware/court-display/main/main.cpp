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

#include <cinttypes>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "board_7b.hpp"
#include "lvgl.h"
#include "padel/application/court_service.hpp"
#include "padel/application/pairing.hpp"
#include "padel/common/log.hpp"
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
constexpr gpio_num_t kBuzzerGpio = static_cast<gpio_num_t>(CONFIG_PADEL_COURT_BUZZER_GPIO);
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

// --- Buzzer + wired buttons (spec 15) ---------------------------------------

esp_timer_handle_t s_buzzer_off_timer = nullptr;

void buzzer_off_cb(void* /*arg*/) {
    gpio_set_level(kBuzzerGpio, 0);
}

void beep(uint32_t duration_ms) {
    gpio_set_level(kBuzzerGpio, 1);
    esp_timer_stop(s_buzzer_off_timer);
    esp_timer_start_once(s_buzzer_off_timer, static_cast<uint64_t>(duration_ms) * 1000);
}

void init_gpio() {
    gpio_config_t out{};
    out.pin_bit_mask = 1ULL << kBuzzerGpio;
    out.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(kBuzzerGpio, 0);

    gpio_config_t in{};
    in.pin_bit_mask = (1ULL << kButtonAGpio) | (1ULL << kButtonBGpio);
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&in));

    const esp_timer_create_args_t off_args = {
        .callback = buzzer_off_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "buzz_off",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&off_args, &s_buzzer_off_timer));
}

// Simple 30 ms press debounce for the arcade buttons; one event per press.
class WiredButton {
public:
    explicit WiredButton(gpio_num_t pin) : pin_(pin) {}

    bool pressed_edge(std::uint64_t now_ms) {
        const bool level = gpio_get_level(pin_) == 0;  // active low
        if (level != raw_) {
            raw_ = level;
            edge_at_ms_ = now_ms;
        }
        if (raw_ != stable_ && now_ms - edge_at_ms_ >= 30) {
            stable_ = raw_;
            if (stable_) {
                return true;
            }
        }
        return false;
    }

private:
    gpio_num_t pin_;
    bool raw_ = false;
    bool stable_ = false;
    std::uint64_t edge_at_ms_ = 0;
};

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
        ShowScreen,
        BeginPairing,
        CancelPairing,
        ConfirmPairing,
        RecoveryChoice,
        TestBeep,
    };
    Type type{};
    TeamId team{TeamId::A};
    std::optional<TeamId> winner{};
    ui::MatchSettings settings{};
    ui::Screen screen{ui::Screen::Setup};
    bool resume = false;
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
    domain::MatchLifecycle prev_lifecycle = domain::MatchLifecycle::NotStarted;
    bool storage_degraded = false;

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
                remember_mac(intent.value().identity.remote_id, frame.mac);
                service->handle_point_intent(intent.value());
            }
        } else if (type.value() == protocol::MessageType::PairRequest) {
            const auto request =
                protocol::parse_pair_request(frame.data, static_cast<size_t>(frame.len));
            if (request) {
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
                beep(80);
            }
        }
    }

    // --- UI commands ---------------------------------------------------------
    void handle_command(const AppCommand& command) {
        using Type = AppCommand::Type;
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
                service->reset_match();
                model.screen = ui::Screen::Setup;
                break;
            case Type::NewMatch:
                model.screen = ui::Screen::Setup;
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
                    beep(150);
                }
                model.screen = ui::Screen::Setup;
                break;
            case Type::RecoveryChoice:
                if (command.resume) {
                    model.screen = ui::Screen::Live;
                    match_started_ms = clock.now_ms();
                } else {
                    fresh_service();
                    model.screen = ui::Screen::Setup;
                }
                break;
            case Type::TestBeep:
                beep(200);
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
        rows.push_back({"Board profile", "Waveshare 7B 1024x600 (UNVERIFIED)"});
        rows.push_back({"Court id", std::to_string(kCourtId)});
        rows.push_back({"Radio channel", std::to_string(CONFIG_PADEL_COURT_WIFI_CHANNEL)});
        rows.push_back({"Accepted", std::to_string(counters.accepted)});
        rows.push_back({"Duplicates", std::to_string(counters.duplicates)});
        rows.push_back({"Rejected", std::to_string(counters.rejected)});
        rows.push_back({"Conflicts", std::to_string(counters.conflicts)});
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

        // Wired backup buttons participate in the conflict guard (spec 15).
        if (button_a.pressed_edge(now)) {
            service->award_point_local(TeamId::A, InputSource::PhysicalBackupButton);
        }
        if (button_b.pressed_edge(now)) {
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

        if (state.lifecycle == domain::MatchLifecycle::Completed &&
            prev_lifecycle != domain::MatchLifecycle::Completed &&
            model.screen == ui::Screen::Live) {
            model.screen = ui::Screen::MatchComplete;
            beep(400);
        }
        prev_lifecycle = state.lifecycle;

        model.settings = settings;
        model.live = ui::build_live_model(*service, settings, now);
        if (now < flash_until_ms) {
            model.live.point_flash = flash_team;
        }
        if (storage_degraded) {
            model.live.storage_fault = true;
        }
        model.complete = ui::build_complete_model(
            *service, settings, match_started_ms > 0 ? now - match_started_ms : 0);

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
    cb.show_screen = [](ui::Screen screen) {
        push_command({.type = AppCommand::Type::ShowScreen, .screen = screen});
    };
    cb.begin_pairing = [](TeamId team) {
        push_command({.type = AppCommand::Type::BeginPairing, .team = team});
    };
    cb.cancel_pairing = []() { push_command({.type = AppCommand::Type::CancelPairing}); };
    cb.confirm_pairing = []() { push_command({.type = AppCommand::Type::ConfirmPairing}); };
    cb.recovery_choice = [](bool resume) {
        push_command({.type = AppCommand::Type::RecoveryChoice, .resume = resume});
    };
    cb.test_beep = []() { push_command({.type = AppCommand::Type::TestBeep}); };
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
    ui::init_theme();

    static ui::CourtUi court_ui;
    court_ui.create(make_callbacks());

    ui::UiModel local_model;
    std::uint64_t last_render_ms = 0;
    while (true) {
        lv_timer_handler();

        const std::uint64_t now = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
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

    static CourtApp app;

    // LVGL on core 1 (rendering-heavy), application on core 0 alongside the
    // Wi-Fi stack so radio->service hand-off stays on one core.
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, nullptr, 4, nullptr, 1);
    xTaskCreatePinnedToCore(app_task, "app", 12288, &app, 5, nullptr, 0);
}
