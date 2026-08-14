// Internal screen structures shared by the per-screen implementation files.
// Not installed; include only from components/ui sources.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "lvgl.h"
#include "padel/ui/court_ui.hpp"
#include "padel/ui/model.hpp"
#include "padel/ui/tokens.hpp"

namespace padel::ui::internal {

// --- Small helpers shared by all screens ------------------------------------

// Sets label text only when it changed (avoids realloc churn on refresh).
void set_text(lv_obj_t* label, const std::string& text);
void set_text(lv_obj_t* label, const char* text);

lv_obj_t* make_screen_root();
lv_obj_t* make_panel(lv_obj_t* parent);
lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color);
lv_obj_t* make_button(lv_obj_t* parent, const char* text, lv_coord_t min_height,
                      lv_event_cb_t handler, void* user_data);

// --- Battery readout ----------------------------------------------------------

// "BAT 79%  ~3h 19m": the percent, with the runtime estimate as a smaller,
// dimmer suffix. The two labels live in their own row container because the
// headers that host it use SPACE_BETWEEN, which would otherwise fling the
// runtime into a slot of its own instead of keeping it next to the percent.
//
// Shared by the live header and match setup so the organizer reads the same
// thing before and during a match.
struct BatteryReadout {
    lv_obj_t* group = nullptr;
    lv_obj_t* percent = nullptr;
    lv_obj_t* runtime = nullptr;
};

BatteryReadout make_battery_readout(lv_obj_t* parent);

// Reads battery_percent / battery_low / battery_runtime. The percent is
// already filtered and the warning already latched upstream; this only
// decides wording and color.
void update_battery_readout(const BatteryReadout& readout, const LiveViewModel& model);

// --- Post-match BACK ----------------------------------------------------------

// Every screen the flow reaches after a match or club mini-set finishes carries
// this button, so a completed score is never a dead end: the domain lets an
// undo through a completed match (ADR-0004) and the host walks the flow back to
// the live board from there. The label spells the effect out because on the mix
// screen a bare "BACK" reads as "back to the summary" rather than "take the
// last point back".
inline constexpr const char* kBackUndoLabel = LV_SYMBOL_LEFT " BACK - UNDO LAST POINT";

template <typename ScreenT>
lv_obj_t* add_back_undo_button(lv_obj_t* parent, ScreenT* screen) {
    return make_button(
        parent, kBackUndoLabel, tokens::kOrganizerTarget,
        [](lv_event_t* e) {
            auto* s = static_cast<ScreenT*>(lv_event_get_user_data(e));
            if (s->shared->callbacks.undo_confirmed) s->shared->callbacks.undo_confirmed();
        },
        screen);
}

// --- Modal confirmation dialog -------------------------------------------------

// A dimmed full-screen backdrop plus a centered card with a title, a message
// and a row of finger-sized buttons. The backdrop swallows the taps that
// would otherwise land on the screen behind it (on the live screen that means
// scoring a point), and tapping it runs on_dismiss.
struct Dialog {
    lv_obj_t* overlay = nullptr;  // delete this to close the dialog
    lv_obj_t* buttons = nullptr;  // button row, filled via add_dialog_button
};

Dialog make_dialog(lv_obj_t* parent, const char* title, const char* message,
                   lv_event_cb_t on_dismiss, void* user_data);
lv_obj_t* add_dialog_button(const Dialog& dialog, const char* text, lv_color_t color,
                            lv_event_cb_t handler, void* user_data);

struct Shared {
    UiCallbacks callbacks{};
    MatchSettings settings_snapshot{};  // last rendered settings (setup screen)
};

// --- Broadcast-style scoreboard ------------------------------------------------

// Two stacked team rows with one cell per set, the way the pro tour overlays
// print them: name plate, a games digit per set, the loser's tiebreak points
// in brackets, the set in progress lit up. Shared by the live screen (where
// it replaces the old games/sets text) and the post-match summary.
struct ScoreboardWidget {
    // Sized for its host: the live screen wants short rows under the score
    // panels, the summary can afford taller ones.
    void create(lv_obj_t* parent, lv_coord_t row_height, const lv_font_t* name_font,
                const lv_font_t* digit_font);
    void update(const ScoreboardModel& model);

    // A five-set match is the longest the domain supports (domain kMaxSets).
    static constexpr int kMaxColumns = 5;

    lv_obj_t* root = nullptr;

    struct Row {
        lv_obj_t* panel = nullptr;
        lv_obj_t* plate = nullptr;      // team-colored name plate
        lv_obj_t* serve_dot = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* cells[kMaxColumns]{};
        lv_obj_t* digits[kMaxColumns]{};
        lv_obj_t* tiebreaks[kMaxColumns]{};
    };
    Row row_a{};
    Row row_b{};
};

// A club round plays two mini-sets with different partners, so the finished
// set cannot become another column of the live board: it needs its own name
// plates. The strip draws the sets already played first, left to right, then
// the board for the set on screen. An ordinary match is a single block.
struct ScoreboardStrip {
    static constexpr int kMaxBlocks = 2;  // a club round is two mini-sets

    void create(lv_obj_t* parent, lv_coord_t row_height, const lv_font_t* name_font,
                const lv_font_t* digit_font);
    void update(const ScoreboardModel& current, const PriorScoreboards& prior);

    lv_obj_t* root = nullptr;  // row of blocks
    ScoreboardWidget blocks[kMaxBlocks]{};
};

// --- Screens ------------------------------------------------------------------

struct LiveScreen {
    void create(Shared* shared);
    void update(const LiveViewModel& model);

    Shared* shared = nullptr;
    lv_obj_t* root = nullptr;

    // header
    lv_obj_t* court_label = nullptr;
    lv_obj_t* mode_label = nullptr;
    lv_obj_t* status_label = nullptr;
    lv_obj_t* radio_label = nullptr;
    BatteryReadout battery{};
    lv_obj_t* storage_label = nullptr;

    struct TeamPanel {
        lv_obj_t* panel = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* players = nullptr;
        lv_obj_t* points = nullptr;
        lv_obj_t* serving_tag = nullptr;
        lv_obj_t* remote_tag = nullptr;
        lv_obj_t* plus_one = nullptr;
    };
    TeamPanel team_a{};
    TeamPanel team_b{};

    ScoreboardStrip scoreboard{};
    lv_obj_t* special_label = nullptr;  // DEUCE / GOLDEN POINT / TIEBREAK

    // conflict banner (spec 12.4)
    lv_obj_t* conflict_banner = nullptr;
    lv_obj_t* conflict_btn_a = nullptr;
    lv_obj_t* conflict_btn_b = nullptr;

    // organizer overlay + dialogs
    lv_obj_t* menu_button = nullptr;
    lv_obj_t* organizer_overlay = nullptr;
    lv_obj_t* brightness_slider = nullptr;
    lv_obj_t* brightness_value_label = nullptr;
    lv_obj_t* pause_button_label = nullptr;
    lv_obj_t* undo_dialog = nullptr;   // created on demand
    lv_obj_t* reset_dialog1 = nullptr;
    lv_obj_t* reset_dialog2 = nullptr;

    std::string undo_preview_text = "Undo the last point?";
    bool undo_available = false;

    void open_organizer_menu();
    void close_organizer_menu();
    void open_undo_dialog();
    void open_reset_dialog(int step);
    void close_dialogs();
};

struct SetupScreen {
    void create(Shared* shared);
    void update(const MatchSettings& settings, const LiveViewModel& live,
                const ClubViewModel& club);
    MatchSettings read_settings() const;

    Shared* shared = nullptr;
    lv_obj_t* root = nullptr;
    lv_obj_t* court_field = nullptr;
    lv_obj_t* team_a_field = nullptr;
    lv_obj_t* team_b_field = nullptr;
    lv_obj_t* preset_dropdown = nullptr;
    lv_obj_t* server_dropdown = nullptr;
    BatteryReadout battery{};
    lv_obj_t* remote_a_status = nullptr;
    lv_obj_t* remote_b_status = nullptr;
    // Shown only while that team has a remote in the allow-list.
    lv_obj_t* unpair_a_button = nullptr;
    lv_obj_t* unpair_b_button = nullptr;
    lv_obj_t* unpair_dialog = nullptr;  // created on demand
    TeamId unpair_target = TeamId::A;
    lv_obj_t* start_label = nullptr;  // START MATCH / START CLUB ROUND
    lv_obj_t* keyboard = nullptr;
    bool fields_initialized = false;

    void open_unpair_dialog(TeamId team);
    void close_dialogs();

    // --- Club round row + player picker modal (screen_club.cpp) -----------
    void create_club_row(lv_obj_t* parent);
    void update_club(const ClubViewModel& club);
    bool club_mode() const;  // MODE dropdown set to "Club round"
    void open_picker(TeamId team);
    void close_picker();
    void rebuild_picker_grid();       // recreates tiles (open / search / roster change)
    void refresh_picker_tiles();      // restyles existing tiles after a pick toggle
    void toggle_pick(const ClubPlayer& player);
    void add_new_player_from_search();
    void add_guest();
    void on_start_pressed();  // routes to start_match or start_club_round

    // Crowns: a double tap in the picker cycles none -> 1 -> 2 -> none. Two
    // players sharing a crown were a Top 2 together, so the round keeps them
    // on opposite sides and out of the same mix pairing.
    // "JOSE [1] & ZOE", or the empty-slot prompt.
    std::string pair_label(const std::vector<ClubPlayer>& picked) const;
    void on_tile_tapped(const ClubPlayer& player);
    bool is_picked(std::uint32_t player_id) const;
    std::uint8_t crown_of(std::uint32_t player_id) const;
    void set_crown(std::uint32_t player_id, std::uint8_t crown);
    void cycle_crown(std::uint32_t player_id);
    void clear_crowns();

    lv_obj_t* club_panel = nullptr;        // players row (all modes)
    lv_obj_t* club_title_label = nullptr;  // retitled per mode
    lv_obj_t* club_pick_a_button = nullptr;
    lv_obj_t* club_pick_b_button = nullptr;
    lv_obj_t* club_a_label = nullptr;      // "JOSE & ZOE" or "tap to pick"
    lv_obj_t* club_b_label = nullptr;
    lv_obj_t* club_hint_label = nullptr;   // forbidden pair / validation

    lv_obj_t* picker_overlay = nullptr;    // created on demand, full-screen
    lv_obj_t* picker_title = nullptr;
    lv_obj_t* picker_hint = nullptr;        // double-tap crown explainer
    lv_obj_t* picker_search = nullptr;
    lv_obj_t* picker_grid = nullptr;
    lv_obj_t* picker_count_label = nullptr;
    lv_obj_t* picker_keyboard = nullptr;

    std::vector<ClubPlayer> roster_snapshot;  // last rendered roster
    std::vector<ClubPlayer> picker_items;     // players behind the grid tiles
    std::vector<lv_obj_t*> picker_crowns;     // crown badge per grid tile
    std::vector<ClubPlayer> picked_a;
    std::vector<ClubPlayer> picked_b;
    // Crown per player id, kept outside the picked lists so deselecting and
    // reselecting a player (which is what a double tap does on the way past)
    // does not lose the mark.
    std::vector<std::pair<std::uint32_t, std::uint8_t>> crowns;
    std::uint32_t last_tap_player = 0;
    std::uint32_t last_tap_ms = 0;
    TeamId picking = TeamId::A;
    int guest_counter = 0;
    std::string pending_new_player;  // auto-select once it lands in the roster
    std::string club_hint_local;     // picker-side validation message
    std::uint32_t applied_suggestion_seq = 0;  // last NEW ROUND seed applied
};

struct SummaryScreen {
    void create(Shared* shared);
    void update(const SummaryViewModel& model);

    // Duration, points won per team, best run: more than that does not fit
    // between the scoreboard and the CONTINUE button.
    static constexpr int kMaxStatRows = 5;

    Shared* shared = nullptr;
    lv_obj_t* root = nullptr;
    lv_obj_t* title_label = nullptr;
    lv_obj_t* winner_label = nullptr;
    ScoreboardStrip scoreboard{};
    struct StatRow {
        lv_obj_t* panel = nullptr;
        lv_obj_t* label = nullptr;
        lv_obj_t* value = nullptr;
    };
    StatRow stats[kMaxStatRows]{};
    lv_obj_t* continue_label = nullptr;
};

struct CompleteScreen {
    void create(Shared* shared);
    void update(const CompleteViewModel& model);

    Shared* shared = nullptr;
    lv_obj_t* root = nullptr;
    lv_obj_t* winner_label = nullptr;
    lv_obj_t* score_label = nullptr;
    lv_obj_t* duration_label = nullptr;
};

struct PairingScreen {
    void create(Shared* shared);
    void update(const PairingViewModel& model);

    Shared* shared = nullptr;
    lv_obj_t* root = nullptr;
    lv_obj_t* team_label = nullptr;
    lv_obj_t* instruction_label = nullptr;
    lv_obj_t* candidate_label = nullptr;
    lv_obj_t* countdown_label = nullptr;
    lv_obj_t* confirm_button = nullptr;
};

struct DiagnosticsScreen {
    void create(Shared* shared);
    void update(const DiagnosticsViewModel& model);

    Shared* shared = nullptr;
    lv_obj_t* root = nullptr;
    lv_obj_t* table = nullptr;
    lv_obj_t* log_label = nullptr;
};

struct RecoveryScreen {
    void create(Shared* shared);
    void update(const RecoveryViewModel& model);

    Shared* shared = nullptr;
    lv_obj_t* root = nullptr;
    lv_obj_t* message_label = nullptr;
    lv_obj_t* detail_label = nullptr;
};

struct ClubMixScreen {
    void create(Shared* shared);
    void update(const ClubViewModel& model);

    Shared* shared = nullptr;
    lv_obj_t* root = nullptr;
    lv_obj_t* detail_label = nullptr;   // set 1 result
    lv_obj_t* team_a_label = nullptr;   // set 2 pairing
    lv_obj_t* team_b_label = nullptr;
};

struct ClubStandingsScreen {
    void create(Shared* shared);
    void update(const ClubViewModel& model);

    Shared* shared = nullptr;
    lv_obj_t* root = nullptr;
    lv_obj_t* coin_label = nullptr;
    struct Row {
        lv_obj_t* panel = nullptr;
        lv_obj_t* rank = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* record = nullptr;
        lv_obj_t* tag = nullptr;  // TOP 2 / BOTTOM 2
    };
    Row rows[4]{};
};

struct Screens {
    Shared shared{};
    LiveScreen live{};
    SetupScreen setup{};
    SummaryScreen summary{};
    CompleteScreen complete{};
    PairingScreen pairing{};
    DiagnosticsScreen diagnostics{};
    RecoveryScreen recovery{};
    ClubMixScreen club_mix{};
    ClubStandingsScreen club_standings{};
    Screen current = Screen::Setup;
    bool created = false;
};

}  // namespace padel::ui::internal
