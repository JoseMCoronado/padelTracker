// Internal screen structures shared by the per-screen implementation files.
// Not installed; include only from components/ui sources.
#pragma once

#include <string>

#include "lvgl.h"
#include "padel/ui/court_ui.hpp"
#include "padel/ui/model.hpp"

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

struct Shared {
    UiCallbacks callbacks{};
    MatchSettings settings_snapshot{};  // last rendered settings (setup screen)
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
    lv_obj_t* storage_label = nullptr;

    struct TeamPanel {
        lv_obj_t* panel = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* players = nullptr;
        lv_obj_t* points = nullptr;
        lv_obj_t* games_sets = nullptr;
        lv_obj_t* serving_tag = nullptr;
        lv_obj_t* remote_tag = nullptr;
        lv_obj_t* plus_one = nullptr;
    };
    TeamPanel team_a{};
    TeamPanel team_b{};

    lv_obj_t* special_label = nullptr;  // DEUCE / GOLDEN POINT / TIEBREAK
    lv_obj_t* history_label = nullptr;
    lv_obj_t* serving_label = nullptr;

    // conflict banner (spec 12.4)
    lv_obj_t* conflict_banner = nullptr;
    lv_obj_t* conflict_btn_a = nullptr;
    lv_obj_t* conflict_btn_b = nullptr;

    // organizer overlay + dialogs
    lv_obj_t* menu_button = nullptr;
    lv_obj_t* organizer_overlay = nullptr;
    lv_obj_t* pause_button_label = nullptr;
    lv_obj_t* undo_dialog = nullptr;   // created on demand
    lv_obj_t* undo_dialog_text = nullptr;
    lv_obj_t* reset_dialog1 = nullptr;
    lv_obj_t* reset_dialog2 = nullptr;

    std::string undo_preview_text = "Undo last point?";

    void open_organizer_menu();
    void close_organizer_menu();
    void open_undo_dialog();
    void open_reset_dialog(int step);
    void close_dialogs();
};

struct SetupScreen {
    void create(Shared* shared);
    void update(const MatchSettings& settings, const LiveViewModel& live);
    MatchSettings read_settings() const;

    Shared* shared = nullptr;
    lv_obj_t* root = nullptr;
    lv_obj_t* court_field = nullptr;
    lv_obj_t* team_a_field = nullptr;
    lv_obj_t* team_b_field = nullptr;
    lv_obj_t* players_a_field = nullptr;
    lv_obj_t* players_b_field = nullptr;
    lv_obj_t* preset_dropdown = nullptr;
    lv_obj_t* server_dropdown = nullptr;
    lv_obj_t* remote_a_status = nullptr;
    lv_obj_t* remote_b_status = nullptr;
    lv_obj_t* keyboard = nullptr;
    bool fields_initialized = false;
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

struct Screens {
    Shared shared{};
    LiveScreen live{};
    SetupScreen setup{};
    CompleteScreen complete{};
    PairingScreen pairing{};
    DiagnosticsScreen diagnostics{};
    RecoveryScreen recovery{};
    Screen current = Screen::Setup;
    bool created = false;
};

}  // namespace padel::ui::internal
