// Desktop court unit: the real CourtService + journal + LVGL UI in an SDL
// window, with the real RemoteCore logic playing the wireless remotes.
// Everything except the SDL backend and the lossy in-process "radio" is the
// same code that ships on the hardware.
//
// Keys:
//   a / b          Team A / Team B remote press (RemoteCore + radio path)
//   A / B (shift)  wired backup button press
//   1 / 2          Team A / Team B remote hold-to-undo (1.5 s hold)
//   l              cycle induced packet loss 0% -> 30% -> 60%
//   p              (on pairing screen) put that team's remote into pairing mode
//   r              power-cycle the court (journal recovery flow)
//   q / ESC        quit

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "lvgl.h"
#include "padel/application/club_controller.hpp"
#include "padel/application/court_service.hpp"
#include "padel/application/roster.hpp"
#include "padel/application/roster_file.hpp"
#include "padel/common/battery.hpp"
#include "padel/common/log.hpp"
#include "padel/application/pairing.hpp"
#include "padel/domain/club_round.hpp"
#include "padel/persistence/journal.hpp"
#include "padel/persistence/stdio_file_backend.hpp"
#include "padel/remote/remote_core.hpp"
#include "padel/sound/tones.hpp"
#include "padel/ui/court_ui.hpp"
#include "padel/ui/model_builder.hpp"
#include "sdl_backend.hpp"

namespace {

using namespace padel;

constexpr CourtId kCourtId = 1;
constexpr std::uint32_t kConflictWindowMs = 250;
const char* kDataDir = "court-sim-data";

struct App;

// One monotonic clock serves both the application and remote interfaces.
class SteadyClock : public application::IClock, public remote::IClock {
public:
    std::uint64_t now_ms() const override {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }
};

// Text-file ISettings so pairings survive simulator restarts.
class FileSettings : public application::ISettings {
public:
    explicit FileSettings(std::string path) : path_(std::move(path)) {}

    std::vector<application::StoredAssignment> load_assignments() override {
        std::vector<application::StoredAssignment> assignments;
        std::ifstream in(path_);
        std::uint32_t remote_id = 0;
        int team = 0;
        while (in >> std::hex >> remote_id >> std::dec >> team) {
            assignments.push_back({remote_id, team == 0 ? TeamId::A : TeamId::B});
        }
        return assignments;
    }

    bool save_assignments(const std::vector<application::StoredAssignment>& assignments) override {
        std::ofstream out(path_, std::ios::trunc);
        for (const auto& assignment : assignments) {
            out << std::hex << assignment.remote_id << ' ' << std::dec
                << (assignment.team == TeamId::A ? 0 : 1) << '\n';
        }
        return static_cast<bool>(out);
    }

private:
    std::string path_;
};

// The simulated remote: real RemoteCore behind in-process radio adapters.
struct SimRemote {
    struct Radio : remote::IRadio {
        App* app = nullptr;
        void send_intent(const protocol::PointIntentPacket& packet) override;
        void send_pair_request(const protocol::PairRequestPacket& packet) override;
    };
    struct Feedback : remote::IFeedback {
        App* app = nullptr;
        const char* label = "?";
        void play(remote::FeedbackPattern pattern) override;
    };
    struct Store : remote::ISettingsStore {
        std::optional<remote::RemoteSettings> stored{};
        std::optional<remote::RemoteSettings> load() override { return stored; }
        bool save(const remote::RemoteSettings& settings) override {
            stored = settings;
            return true;
        }
    };

    std::uint32_t device_id = 0;
    Radio radio{};
    Feedback feedback{};
    Store store{};
    std::unique_ptr<remote::RemoteCore> core{};
    std::uint64_t release_at_ms = 0;  // scheduled button release

    // Held past RemoteCoreConfig::stable_press_ms so a keypress behaves like
    // a deliberate press rather than the brush the debounce now rejects.
    void press(std::uint64_t now) {
        core->set_button_level(true);
        release_at_ms = now + 250;
    }

    // Long enough for RemoteCore to turn the hold into an undo (ADR-0014).
    void hold(std::uint64_t now, std::uint32_t ms) {
        core->set_button_level(true);
        release_at_ms = now + ms;
    }
};

// Games played in the set under way. A finished game shows up here as a step
// up, which is how the buzzer hears one: the reducer stores no GameWon event
// (ADR-0003).
std::uint8_t games_in_current_set(const domain::MatchState& state) {
    return static_cast<std::uint8_t>(state.current_set.games_a + state.current_set.games_b);
}

struct App {
    SteadyClock clock{};
    sim::SdlBackend backend_sdl{};
    ui::CourtUi court_ui{};
    ui::UiModel model{};
    ui::MatchSettings settings{};

    std::unique_ptr<persistence::StdioFileBackend> file;
    std::unique_ptr<persistence::JournalEventStore> store;
    std::unique_ptr<application::CourtService> service;
    std::unique_ptr<FileSettings> court_settings;
    std::unique_ptr<application::PairingService> pairing;

    // Club round: persistent roster + results, controller over the match flow.
    std::unique_ptr<application::FileRosterStore> roster_store;
    std::unique_ptr<application::PlayerRoster> roster;
    std::unique_ptr<application::FileResultsLog> results_log;
    std::unique_ptr<application::ClubController> club;
    bool club_active = false;
    std::string club_hint;
    std::vector<ui::ClubPlayer> club_suggested_a;
    std::vector<ui::ClubPlayer> club_suggested_b;
    std::uint32_t club_suggestion_seq = 0;

    SimRemote remote_a{};
    SimRemote remote_b{};
    std::mt19937 rng{0xC0FFEE};
    int loss_pct = 0;

    std::uint64_t match_started_ms = 0;
    std::uint64_t boot_ms = 0;
    std::optional<TeamId> flash_team{};
    std::uint64_t flash_until_ms = 0;
    std::uint8_t prev_points_a = 0;
    std::uint8_t prev_points_b = 0;
    std::uint8_t prev_completed_sets = 0;
    std::uint8_t prev_games_in_set = 0;
    domain::MatchLifecycle prev_lifecycle = domain::MatchLifecycle::NotStarted;
    std::uint32_t acked_remote_undos = 0;
    std::vector<std::string> log_lines;

    // Simulated court Li-ion (CITYORK 2000 mAh on the Waveshare PH2.0).
    std::uint16_t battery_mv = 3900;
    std::uint8_t brightness_percent = 100;

    void log(const std::string& line) {
        std::printf("[sim] %s\n", line.c_str());
        log_lines.push_back(line);
        if (log_lines.size() > 12) {
            log_lines.erase(log_lines.begin());
        }
    }

    // The desktop has no buzzer, so a cue is printed with the notes the court
    // unit would play. Same table, same call sites — the sound design can be
    // reviewed here before hardware exists.
    void play_cue(sound::Cue cue) {
        const sound::Pattern pattern = sound::pattern_for(cue);
        std::string notes;
        for (std::size_t i = 0; i < pattern.count; ++i) {
            const sound::Tone tone = pattern.tones[i];
            notes += (i > 0 ? " " : "");
            notes += (tone.freq_hz == 0 ? std::string("rest")
                                        : std::to_string(tone.freq_hz) + "Hz");
            notes += "/" + std::to_string(tone.duration_ms) + "ms";
        }
        log("buzzer: " + std::string(pattern.name) + " [" + notes + "]");
    }

    bool lost() { return loss_pct > 0 && static_cast<int>(rng() % 100) < loss_pct; }

    std::string journal_path() const { return std::string(kDataDir) + "/journal.bin"; }

    // --- Radio delivery (court side) ----------------------------------------
    void deliver_intent(const protocol::PointIntentPacket& packet) {
        if (!lost()) {
            service->handle_point_intent(packet);
        }
    }

    void deliver_pair_request(const protocol::PairRequestPacket& packet) {
        if (pairing && !lost()) {
            pairing->handle_pair_request(packet);
        }
    }

    void remote_feedback(const char* label, remote::FeedbackPattern pattern) {
        switch (pattern) {
            case remote::FeedbackPattern::PressRegistered:
                break;  // LED blink; too chatty for the log
            case remote::FeedbackPattern::Accepted:
                log(std::string("remote ") + label + ": confirmed (green flash)");
                break;
            case remote::FeedbackPattern::RejectedConflict:
                log(std::string("remote ") + label + ": conflict feedback");
                break;
            case remote::FeedbackPattern::RejectedOther:
                log(std::string("remote ") + label + ": rejected (amber)");
                break;
            case remote::FeedbackPattern::CommFailed:
                log(std::string("remote ") + label + ": FAILED after retries (red)");
                break;
            case remote::FeedbackPattern::UndoSent:
                log(std::string("remote ") + label + ": hold -> undo sent");
                break;
            case remote::FeedbackPattern::PairingRequired:
                log(std::string("remote ") + label + ": not paired");
                break;
            case remote::FeedbackPattern::PairingSuccess:
                log(std::string("remote ") + label + ": PAIRED (green sequence)");
                break;
            case remote::FeedbackPattern::Woke:
                log(std::string("remote ") + label + ": woke from sleep (press ignored)");
                break;
        }
    }

    // --- Boot / reboot -----------------------------------------------------
    void setup_remotes() {
        remote_a.device_id = 0xA1;
        remote_b.device_id = 0xB1;
        remote_a.feedback.label = "A";
        remote_b.feedback.label = "B";
        for (SimRemote* remote : {&remote_a, &remote_b}) {
            remote->radio.app = this;
            remote->feedback.app = this;
            remote->core = std::make_unique<remote::RemoteCore>(
                remote::RemoteCoreConfig{}, clock, remote->radio, remote->feedback,
                remote->store);
            remote->core->begin(static_cast<std::uint32_t>(rng()), remote->device_id);
            remote->core->set_battery_mv(3900);
        }
    }

    void sync_remotes_to_assignments() {
        // Mirror the court's persisted allow-list into the sim remotes'
        // own persistence (as if they had been paired physically).
        for (const auto& assignment : court_settings->load_assignments()) {
            for (SimRemote* remote : {&remote_a, &remote_b}) {
                if (remote->device_id == assignment.remote_id &&
                    !remote->core->settings().paired) {
                    remote->core->apply_pairing(assignment.remote_id, kCourtId,
                                                assignment.team);
                }
            }
        }
    }

    void create_pairing_service() {
        pairing = std::make_unique<application::PairingService>(
            application::PairingService::Config{kCourtId, 1, 30'000}, *service,
            *court_settings, clock);
        pairing->load_assignments();
    }

    void boot() {
        std::filesystem::create_directories(kDataDir);
        court_settings = std::make_unique<FileSettings>(std::string(kDataDir) + "/pairings.txt");

        // Club roster + results (kept across sim power cycles; an in-flight
        // club round is RAM-only and is lost, like on real hardware).
        if (!roster) {
            roster_store = std::make_unique<application::FileRosterStore>(
                std::string(kDataDir) + "/roster.txt");
            roster = std::make_unique<application::PlayerRoster>(*roster_store);
            results_log = std::make_unique<application::FileResultsLog>(
                std::string(kDataDir) + "/club_results.csv");
            club = std::make_unique<application::ClubController>(*results_log, clock);
        }
        // First run: seed default assignments so the keyboard remotes work
        // out of the box; the pairing flow can re-pair at any time.
        if (court_settings->load_assignments().empty()) {
            court_settings->save_assignments({{0xA1, TeamId::A}, {0xB1, TeamId::B}});
        }

        file = std::make_unique<persistence::StdioFileBackend>(journal_path());
        const persistence::RecoveryResult recovered = persistence::recover(file->read_all());
        file->truncate(recovered.valid_bytes);
        logging::emit(logging::Level::Info, "storage.recovered", "events=%zu tail=%s",
                      recovered.events.size(),
                      recovered.tail == persistence::TailStatus::Clean ? "clean" : "damaged");
        store = std::make_unique<persistence::JournalEventStore>(*file, recovered.valid_bytes);
        service = std::make_unique<application::CourtService>(
            application::CourtServiceConfig{kCourtId, kConflictWindowMs},
            ui::preset_config(settings.preset_index), recovered.events, *store, clock);
        create_pairing_service();
        sync_remotes_to_assignments();
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
                          recovered.events.size(),
                          recovered.tail == persistence::TailStatus::Clean ? "clean"
                                                                            : "damaged (truncated)");
            model.recovery.detail = detail;
            model.recovery.corrupt_tail = recovered.tail != persistence::TailStatus::Clean;
            log("boot: resumable match found, showing recovery screen");
        } else {
            model.screen = ui::Screen::Setup;
            log("boot: no resumable match, showing setup");
        }
    }

    void archive_journal() {
        pairing.reset();
        service.reset();
        store.reset();
        file.reset();
        const std::string archived = std::string(kDataDir) + "/journal-" +
                                     std::to_string(clock.now_ms()) + ".bin";
        std::error_code ec;
        std::filesystem::rename(journal_path(), archived, ec);
        if (!ec) {
            log("journal archived to " + archived);
        }
    }

    void fresh_service(const domain::MatchConfig& config) {
        file = std::make_unique<persistence::StdioFileBackend>(journal_path());
        store = std::make_unique<persistence::JournalEventStore>(*file, 0);
        service = std::make_unique<application::CourtService>(
            application::CourtServiceConfig{kCourtId, kConflictWindowMs}, config, *store, clock);
        create_pairing_service();
    }

    void power_cycle() {
        log("=== POWER CYCLE (court only; remotes stay alive) ===");
        boot();
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

    // Starts the mini-set the club controller is waiting on (set 1 or the
    // mixed set 2) as a normal journaled match with the pairing as team names.
    void begin_club_set() {
        const auto teams = club->current_set_teams();
        settings.team_a_name = teams.team_a;
        settings.team_b_name = teams.team_b;
        settings.players_a.clear();
        settings.players_b.clear();
        settings.preset_index = ui::kClubRoundPreset;

        if (service->state().lifecycle != domain::MatchLifecycle::NotStarted ||
            file->size() > 0) {
            archive_journal();
            fresh_service(domain::preset_club_mini_set());
        }
        if (service->start_match(settings.first_server)) {
            match_started_ms = clock.now_ms();
            model.screen = ui::Screen::Live;
            log("club set " + std::to_string(club->set_number()) + ": " + teams.team_a +
                " vs " + teams.team_b);
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
        if (club->stage() == domain::ClubStage::Set2) {
            model.screen = ui::Screen::ClubMix;
            log("club set 1 done -> mix: " + club->current_set_teams().team_a + " vs " +
                club->current_set_teams().team_b);
        } else {
            model.screen = ui::Screen::ClubStandings;
            if (!club->coin_flip_announcement().empty()) {
                log(club->coin_flip_announcement());
            }
        }
    }

    // --- UI callbacks --------------------------------------------------------
    ui::UiCallbacks callbacks() {
        ui::UiCallbacks cb{};
        cb.award_point = [this](TeamId team) {
            const auto result = service->award_point_local(team, InputSource::TouchscreenAdmin);
            if (result.outcome != application::LocalPointOutcome::Committed) {
                log("touch +1 rejected");
            }
        };
        cb.undo_confirmed = [this]() {
            if (!service->undo_last_scoring_action()) {
                log("undo rejected");
            }
        };
        cb.toggle_pause = [this]() {
            if (service->state().lifecycle == domain::MatchLifecycle::Paused) {
                service->resume_match();
            } else {
                service->pause_match();
            }
        };
        cb.resolve_conflict = [this](std::optional<TeamId> winner) {
            service->resolve_conflict(winner);
            log(winner ? "conflict resolved" : "conflict cancelled");
        };
        cb.start_match = [this](const ui::MatchSettings& s) {
            abandon_club_round();
            settings = s;
            // A fresh match genesis needs a fresh journal; archive the old
            // one per spec 14.7.
            if (service->state().lifecycle != domain::MatchLifecycle::NotStarted ||
                file->size() > 0) {
                archive_journal();
                fresh_service(ui::preset_config(settings.preset_index));
            }
            if (service->start_match(settings.first_server)) {
                match_started_ms = clock.now_ms();
                model.screen = ui::Screen::Live;
                log("match started");
            }
        };
        cb.reset_confirmed = [this]() {
            abandon_club_round();
            service->reset_match();
            model.screen = ui::Screen::Setup;
            log("match reset (journaled; archived on next start)");
        };
        cb.new_match = [this]() {
            abandon_club_round();
            model.screen = ui::Screen::Setup;
        };
        cb.summary_continue = [this]() { advance_past_summary(); };
        cb.show_screen = [this](ui::Screen screen) { model.screen = screen; };
        cb.begin_pairing = [this](TeamId team) {
            pairing->begin(team);
            model.screen = ui::Screen::Pairing;
            log(std::string("pairing window open for TEAM ") + (team == TeamId::A ? "A" : "B"));
        };
        cb.cancel_pairing = [this]() {
            pairing->cancel();
            model.screen = ui::Screen::Setup;
        };
        cb.confirm_pairing = [this]() {
            if (const auto assign = pairing->confirm()) {
                // Broadcast: both remotes hear it, only the target applies it.
                remote_a.core->on_pair_assign(*assign);
                remote_b.core->on_pair_assign(*assign);
                log("pairing confirmed, PAIR_ASSIGN sent");
                play_cue(sound::Cue::PairingConfirmed);
            }
            model.screen = ui::Screen::Setup;
        };
        cb.unpair_remote = [this](TeamId team) {
            // The allow-list permits several remotes per team, so drain the
            // team rather than dropping only the one the UI showed. The remote
            // keeps its own credentials until the court rejects its next
            // press, exactly as on hardware.
            while (const auto info = service->remote_info(team)) {
                pairing->unassign(info->remote_id);
            }
            log(std::string("TEAM ") + (team == TeamId::A ? "A" : "B") + " remote unpaired");
        };
        cb.recovery_choice = [this](bool resume) {
            if (resume) {
                model.screen = ui::Screen::Live;
                log("recovery: match resumed");
            } else {
                abandon_club_round();
                archive_journal();
                fresh_service(ui::preset_config(settings.preset_index));
                model.screen = ui::Screen::Setup;
                log("recovery: match discarded (journal archived)");
            }
        };
        cb.test_beep = [this]() { play_cue(sound::Cue::SelfTest); };
        cb.set_brightness = [this](std::uint8_t percent) {
            brightness_percent = percent;
            log("brightness " + std::to_string(percent) + "%");
        };

        // --- Club round -----------------------------------------------------
        cb.create_player = [this](const std::string& name) {
            if (const auto player = roster->add_player(name)) {
                log("player added: " + player->name);
            } else {
                log("player rejected (empty or duplicate): " + name);
            }
        };
        cb.start_club_round = [this](const std::array<ui::ClubPlayer, 4>& picked,
                                     const ui::MatchSettings& s) {
            // Setup always means start fresh; orphaned rounds (RESET / NEW
            // MATCH without finish) must not block the next start.
            if (club->round_active()) {
                abandon_club_round();
            }
            settings = s;
            std::array<application::Player, 4> players{};
            for (std::size_t i = 0; i < 4; ++i) {
                players[i] = application::Player{picked[i].id, picked[i].name, picked[i].guest};
            }
            const auto error = club->start_round(players, static_cast<std::uint32_t>(rng()),
                                                 ui::crown_pairs(picked));
            if (error == application::ClubController::StartError::ForbiddenPair) {
                club_hint =
                    "Crowned pairs can't be teammates - put them on opposite sides";
                log("club round rejected: forbidden pair");
                return;
            }
            if (error == application::ClubController::StartError::DuplicatePlayer) {
                club_hint = "Same player picked twice - split them up";
                return;
            }
            if (error.has_value()) {
                club_hint = "A club round is already in progress";
                return;
            }
            club_hint.clear();
            club_active = true;
            begin_club_set();
        };
        cb.club_next_set = [this]() {
            if (club_active && club->stage() == domain::ClubStage::Set2) {
                begin_club_set();
            }
        };
        cb.club_new_round = [this]() {
            // Capture Top 2 / Bottom 2 before finish_round clears the round.
            if (ui::suggest_next_round_picks(*club, club_suggested_a, club_suggested_b)) {
                ++club_suggestion_seq;
            }
            club->finish_round();
            club_active = false;
            club_hint.clear();
            model.screen = ui::Screen::Setup;
            log("club round closed; Top 2 alone on opposite teams (pick partners)");
        };
        cb.club_done = [this]() {
            club->finish_round();
            club_active = false;
            club_hint.clear();
            model.screen = ui::Screen::Setup;
        };
        return cb;
    }

    // --- Frame -------------------------------------------------------------
    void handle_key(const sim::KeyEvent& key, std::uint64_t now) {
        switch (key.keycode) {
            case SDLK_a:
                if (key.shift) {
                    service->award_point_local(TeamId::A, InputSource::PhysicalBackupButton);
                    log("wired button: Team A");
                } else {
                    remote_a.press(now);
                }
                break;
            case SDLK_b:
                if (key.shift) {
                    service->award_point_local(TeamId::B, InputSource::PhysicalBackupButton);
                    log("wired button: Team B");
                } else {
                    remote_b.press(now);
                }
                break;
            case SDLK_1:
                remote_a.hold(now, 1700);
                log("remote A: holding to undo");
                break;
            case SDLK_2:
                remote_b.hold(now, 1700);
                log("remote B: holding to undo");
                break;
            case SDLK_l:
                loss_pct = loss_pct == 0 ? 30 : loss_pct == 30 ? 60 : 0;
                log("induced loss now " + std::to_string(loss_pct) + "%");
                break;
            case SDLK_p:
                if (model.screen == ui::Screen::Pairing && pairing->team()) {
                    SimRemote& remote =
                        *pairing->team() == TeamId::A ? remote_a : remote_b;
                    remote.core->clear_pairing();
                    remote.core->enter_pairing_mode();
                    log("remote entered pairing mode (advertising)");
                }
                break;
            case SDLK_r:
                power_cycle();
                break;
            default:
                break;
        }
    }

    void build_diagnostics(std::uint64_t now) {
        auto& rows = model.diagnostics.rows;
        rows.clear();
        const auto& counters = service->counters();
        const auto& dedup = service->deduplicator().counters();
        const auto remote_row = [&](const SimRemote& remote, const char* label) {
            const auto& s = remote.core->settings();
            std::string value = s.paired
                                    ? std::string("paired, team ") +
                                          (s.team == TeamId::A ? "A" : "B")
                                    : "unpaired";
            value += ", confirmed " + std::to_string(remote.core->stats().confirmed);
            rows.push_back({label, value});
        };
        rows.push_back({"Firmware", "native court-sim"});
        rows.push_back({"Board profile", "SDL 1024x600"});
        rows.push_back({"Court id", std::to_string(kCourtId)});
        rows.push_back({"Radio channel", "n/a (simulated)"});
        const auto soc = padel::battery::mv_to_percent(battery_mv);
        if (soc) {
            rows.push_back({"Battery", std::to_string(*soc) + "%"});
        } else {
            rows.push_back({"Battery", "unknown / no cell"});
        }
        {
            char volt[16];
            std::snprintf(volt, sizeof(volt), "%u.%02u V", battery_mv / 1000u,
                          (battery_mv % 1000u) / 10u);
            rows.push_back({"Battery voltage", volt});
        }
        rows.push_back({"Est. runtime", padel::battery::format_runtime_estimate(soc)});
        rows.push_back({"Brightness", std::to_string(brightness_percent) + "%"});
        remote_row(remote_a, "Remote 0xA1");
        remote_row(remote_b, "Remote 0xB1");
        rows.push_back({"Induced loss", std::to_string(loss_pct) + "%"});
        rows.push_back({"Accepted", std::to_string(counters.accepted)});
        rows.push_back({"Duplicates", std::to_string(counters.duplicates)});
        rows.push_back({"Rejected", std::to_string(counters.rejected)});
        rows.push_back({"Conflicts", std::to_string(counters.conflicts)});
        rows.push_back({"Remote undos", std::to_string(counters.remote_undos)});
        rows.push_back({"Storage failures", std::to_string(counters.storage_failures)});
        rows.push_back({"Dedup accepted/dup/stale",
                        std::to_string(dedup.accepted) + "/" + std::to_string(dedup.duplicates) +
                            "/" + std::to_string(dedup.stale)});
        rows.push_back({"Journal size", std::to_string(file->size()) + " B"});
        rows.push_back({"State revision", std::to_string(service->state().revision)});
        rows.push_back({"Log records", std::to_string(logging::total_emitted())});
        rows.push_back({"Uptime", std::to_string((now - boot_ms) / 1000) + " s"});
        // Structured event ring (spec 16); sim UX notes go to stdout only.
        model.diagnostics.recent_log_lines = logging::recent_lines(10);
    }

    void frame(std::uint64_t now) {
        // Remotes: scheduled button releases + state machines.
        for (SimRemote* remote : {&remote_a, &remote_b}) {
            if (remote->release_at_ms != 0 && now >= remote->release_at_ms) {
                remote->core->set_button_level(false);
                remote->release_at_ms = 0;
            }
            remote->core->poll();
        }

        service->tick();
        if (pairing) {
            pairing->tick();
        }

        // ACK path back to the remotes (lossy). The buzzer cue is not lossy:
        // it happens on the court unit, whether or not the ACK arrives.
        for (const protocol::AckPacket& ack : service->drain_acks()) {
            if (ack.status == protocol::AckStatus::Accepted) {
                play_cue(sound::Cue::PointScored);
            }
            if (lost()) {
                continue;
            }
            remote_a.core->on_ack(ack);
            remote_b.core->on_ack(ack);
        }
        const std::uint32_t undos = service->counters().remote_undos;
        if (undos > acked_remote_undos) {
            play_cue(sound::Cue::RemoteUndo);
        }
        acked_remote_undos = undos;  // a rebuilt service restarts at zero

        // Point flash: detect committed points regardless of input path.
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

        // Natural completion moves to the match complete screen (edge only,
        // so REVIEW / CORRECT can go back to the live screen). In a club
        // round the finished mini-set feeds the controller instead, and the
        // flow continues on the mix or standings screen.
        //
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
            if (club_active) {
                club->on_set_complete(state);
            }
            model.screen = ui::Screen::MatchSummary;
            play_cue(sound::Cue::MatchComplete);
        } else if (prev_lifecycle == domain::MatchLifecycle::Completed &&
                   state.lifecycle != domain::MatchLifecycle::Completed &&
                   ui::is_post_match_screen(model.screen)) {
            // An undo took the winning point back: walk the whole flow back
            // to the live screen, club round included.
            if (club_active) {
                club->undo_last_set();
            }
            model.screen = ui::Screen::Live;
            log("undo reopened the match");
        } else if (!match_completed_edge && !rewound) {
            if (completed_sets > prev_completed_sets) {
                play_cue(sound::Cue::SetComplete);
            } else if (games_in_set > prev_games_in_set) {
                play_cue(sound::Cue::GameComplete);
            }
        }
        prev_lifecycle = state.lifecycle;
        prev_completed_sets = completed_sets;
        prev_games_in_set = games_in_set;

        // Assemble the frame model.
        model.settings = settings;
        model.live = ui::build_live_model(*service, settings, now);
        model.live.brightness_percent = brightness_percent;
        model.live.battery_percent = padel::battery::mv_to_percent(battery_mv);
        if (now < flash_until_ms) {
            model.live.point_flash = flash_team;
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
                model.screen = ui::Screen::Setup;  // window expired or confirmed
            } else {
                const TeamId team = *pairing->team();
                model.pairing.team_label =
                    std::string("Pairing: TEAM ") + (team == TeamId::A ? "A" : "B");
                model.pairing.instruction =
                    "Hold the remote button for 5 seconds to enter pairing mode.\n"
                    "(Simulator: press P)";
                const auto& candidate = pairing->candidate();
                model.pairing.candidate_label =
                    candidate ? "Remote " + candidate->short_id + " requests pairing" : "";
                model.pairing.awaiting_confirm = candidate.has_value();
                model.pairing.seconds_left = static_cast<int>(pairing->seconds_left());
            }
        }
        build_diagnostics(now);

        court_ui.render(model);
    }
};

void SimRemote::Radio::send_intent(const protocol::PointIntentPacket& packet) {
    app->deliver_intent(packet);
}

void SimRemote::Radio::send_pair_request(const protocol::PairRequestPacket& packet) {
    app->deliver_pair_request(packet);
}

void SimRemote::Feedback::play(remote::FeedbackPattern pattern) {
    app->remote_feedback(label, pattern);
}

// Scripted screenshot tour (spec 18.6 subset): renders every screen at
// 1024x600 with stress content (long names, special scoring states, fault
// banners) and saves BMPs for review.
int run_tour(App& app, const std::string& out_dir) {
    std::filesystem::create_directories(out_dir);
    const auto settle_and_shoot = [&](const char* name) {
        for (int i = 0; i < 30; ++i) {
            lv_tick_inc(16);
            lv_timer_handler();
        }
        const std::string path = out_dir + "/" + name + ".bmp";
        if (!app.backend_sdl.screenshot(path.c_str())) {
            std::fprintf(stderr, "screenshot failed: %s\n", path.c_str());
            return false;
        }
        std::printf("saved %s\n", path.c_str());
        return true;
    };

    ui::UiModel m{};

    // Stress content per spec 18.6: long names everywhere.
    ui::TeamPanelModel long_a{};
    long_a.name = "LOS GUERREROS DEL PADEL";
    long_a.players = "MAXIMILIANO / ALEJANDRO";
    long_a.points = "40";
    long_a.games = "5";
    long_a.sets = "1";
    long_a.serving = true;
    long_a.remote_assigned = true;
    long_a.remote_ok = true;
    ui::TeamPanelModel long_b = long_a;
    long_b.name = "CLUB DEPORTIVO CAMPEONES";
    long_b.players = "SEBASTIAN / MAXIMILIANO";
    long_b.points = "AD";
    long_b.games = "6";
    long_b.sets = "1";
    long_b.serving = false;
    long_b.remote_ok = false;

    m.screen = ui::Screen::Setup;
    m.live.team_a = long_a;
    m.live.team_b = long_b;
    app.court_ui.render(m);
    if (!settle_and_shoot("01-setup")) return 1;

    m.screen = ui::Screen::Live;
    m.live.court_label = "COURT 12 - CENTER";
    m.live.mode_label = "STANDARD / ADV / MTB";
    m.live.status_label = "LIVE";
    m.live.scoreboard.name_a = "MAXIMILIANO / ALEJANDRO";
    m.live.scoreboard.name_b = "SEBASTIAN / MAXIMILIANO";
    m.live.scoreboard.serving = TeamId::A;
    m.live.scoreboard.columns = {{"7", "6", "", "(5)", false, TeamId::A},
                                 {"4", "6", "", "", false, TeamId::B},
                                 {"5", "6", "", "", true, std::nullopt}};
    m.live.special_label = "";
    m.live.radio_ok = true;
    m.live.revision = 214;
    app.court_ui.render(m);
    if (!settle_and_shoot("02-live-long-names")) return 1;

    m.live.special_label = "GOLDEN POINT";
    m.live.team_a.points = "40";
    m.live.team_b.points = "40";
    m.live.conflict = true;
    m.live.point_flash = TeamId::A;
    app.court_ui.render(m);
    if (!settle_and_shoot("03-live-golden-conflict")) return 1;

    m.live.conflict = false;
    m.live.point_flash.reset();
    m.live.special_label = "MATCH TIEBREAK";
    m.live.team_a.points = "12";
    m.live.team_b.points = "11";
    m.live.paused = true;
    m.live.status_label = "PAUSED";
    m.live.storage_fault = true;
    m.live.radio_ok = false;
    app.court_ui.render(m);
    if (!settle_and_shoot("04-live-tiebreak-paused-faults")) return 1;

    m.screen = ui::Screen::MatchComplete;
    m.complete.winner_label = "LOS GUERREROS DEL PADEL WIN";
    m.complete.final_score = "7-6(5)  4-6  7-5";
    m.complete.duration_label = "Duration: 96 min";
    app.court_ui.render(m);
    if (!settle_and_shoot("05-complete")) return 1;

    m.screen = ui::Screen::Pairing;
    m.pairing.team_label = "Pairing: TEAM A";
    m.pairing.instruction = "Hold the remote button for 5 seconds to enter pairing mode.";
    m.pairing.candidate_label = "Remote 3F92 requests pairing";
    m.pairing.awaiting_confirm = true;
    m.pairing.seconds_left = 22;
    app.court_ui.render(m);
    if (!settle_and_shoot("06-pairing")) return 1;

    m.screen = ui::Screen::Diagnostics;
    m.diagnostics.rows = {
        {"Firmware", "native court-sim"},   {"Board profile", "SDL 1024x600"},
        {"Court id", "1"},                  {"Accepted", "1042"},
        {"Duplicates", "97"},               {"Rejected", "3"},
        {"Conflicts", "2"},                 {"Storage failures", "0"},
        {"Journal size", "18324 B"},        {"State revision", "214"},
    };
    m.diagnostics.recent_log_lines = {"match.point_accepted team=A rev=214",
                                      "radio.duplicate remote=0xA1 seq=502",
                                      "match.conflict_opened window=250ms"};
    app.court_ui.render(m);
    if (!settle_and_shoot("07-diagnostics")) return 1;

    m.screen = ui::Screen::Recovery;
    m.recovery.message = "A match was in progress when power was lost.";
    m.recovery.detail = "Journal: 214 events recovered, tail damaged (truncated).";
    m.recovery.corrupt_tail = true;
    app.court_ui.render(m);
    if (!settle_and_shoot("08-recovery")) return 1;

    m.club.roster = {{1, "Jose", false},    {2, "Zoe", false},
                     {3, "William", false}, {4, "Szewei", false},
                     {5, "Ruxandra", false},{6, "Lewis", false},
                     {7, "Luigi", false},   {8, "Raymond", false},
                     {9, "Paulina", false}, {10, "Vineet", false},
                     {11, "Louis", false},  {12, "Adrien", false}};
    m.club.setup_hint =
        "Last round's Top 2 can't be teammates - put them on opposite sides";
    m.screen = ui::Screen::Setup;
    app.court_ui.debug_select_preset(ui::kClubRoundPreset);  // MODE = Club round
    app.court_ui.render(m);
    if (!settle_and_shoot("09-setup-club-hint")) return 1;

    app.court_ui.debug_open_club_picker(TeamId::A);
    app.court_ui.render(m);
    if (!settle_and_shoot("10-club-picker")) return 1;
    // Leave the modal open state behind before the next screens.
    m.screen = ui::Screen::Live;
    app.court_ui.render(m);

    m.screen = ui::Screen::ClubMix;
    m.club.mix_detail = "ADRIEN & LEWIS took set 1 (3-1)";
    m.club.mix_team_a = "ADRIEN & LOUIS";
    m.club.mix_team_b = "LEWIS & LUIGI";
    app.court_ui.render(m);
    if (!settle_and_shoot("11-club-mix")) return 1;

    m.screen = ui::Screen::ClubStandings;
    m.club.standings = {{"1", "Adrien", "2 WINS  +4", true, false},
                        {"2", "Lewis", "1 WIN  +0", true, true},
                        {"3", "Louis", "1 WIN  +0", false, false},
                        {"4", "Luigi", "0 WINS  -4", false, false}};
    m.club.coin_announcement = "COIN FLIP: LEWIS takes the last TOP 2 spot";
    app.court_ui.render(m);
    if (!settle_and_shoot("12-club-standings")) return 1;

    m.screen = ui::Screen::MatchSummary;
    m.summary.title = "CLUB SET 1 COMPLETE";
    m.summary.winner_label = "LOS GUERREROS DEL PADEL WIN";
    m.summary.scoreboard = m.live.scoreboard;
    m.summary.scoreboard.serving.reset();
    m.summary.stats = {{"Duration", "96 min"},
                       {"Points played", "184"},
                       {"MAXIMILIANO / ALEJANDRO", "97  (53%)"},
                       {"SEBASTIAN / MAXIMILIANO", "87  (47%)"},
                       {"Best run - MAXIMILIANO / ALEJANDRO", "7 in a row"}};
    m.summary.continue_label = "MIX IT UP";
    app.court_ui.render(m);
    if (!settle_and_shoot("13-match-summary")) return 1;

    // Switching screens dismisses the menu, so land on live first.
    m.screen = ui::Screen::Live;
    app.court_ui.render(m);
    app.court_ui.debug_open_organizer_menu(true);
    app.court_ui.render(m);
    if (!settle_and_shoot("14-live-organizer")) return 1;
    app.court_ui.debug_open_organizer_menu(false);

    m.live.undo_preview = TeamId::A;
    app.court_ui.render(m);
    app.court_ui.debug_open_undo_dialog();
    app.court_ui.render(m);
    if (!settle_and_shoot("15-undo-dialog")) return 1;

    app.court_ui.debug_open_reset_dialog(2);
    app.court_ui.render(m);
    if (!settle_and_shoot("16-reset-confirm")) return 1;

    return 0;
}

}  // namespace

namespace {

std::uint64_t log_clock() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void log_sink(const logging::Entry& entry) {
    std::printf("[log %8llu] %s %s\n", static_cast<unsigned long long>(entry.t_ms),
                entry.event, entry.detail);
}

}  // namespace

int main(int argc, char** argv) {
    logging::set_clock(log_clock);
    logging::set_sink(log_sink);
    App app;
    if (!app.backend_sdl.init(1024, 600, "Padel Smart Court - simulator")) {
        std::fprintf(stderr, "SDL init failed\n");
        return 1;
    }
    ui::init_theme();
    app.court_ui.create(app.callbacks());

    if (argc >= 2 && std::string(argv[1]) == "--tour") {
        const int result = run_tour(app, argc >= 3 ? argv[2] : "court-sim-shots");
        app.backend_sdl.shutdown();
        return result;
    }

    app.setup_remotes();
    app.boot();

    std::printf(
        "Padel court simulator\n"
        "  a/b: remote press   Shift+a/b: wired button   l: loss   p: pair   r: reboot   q: "
        "quit\n");

    std::vector<sim::KeyEvent> keys;
    bool running = true;
    while (running) {
        keys.clear();
        running = app.backend_sdl.pump(keys);
        const std::uint64_t now = app.clock.now_ms();
        for (const sim::KeyEvent& key : keys) {
            if (key.keycode == SDLK_q || key.keycode == SDLK_ESCAPE) {
                running = false;
            }
            app.handle_key(key, now);
        }
        app.frame(now);
        lv_timer_handler();
        SDL_Delay(5);
    }
    app.backend_sdl.shutdown();
    return 0;
}
