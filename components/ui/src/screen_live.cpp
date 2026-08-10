// Live match screen (spec 14.3) plus the organizer overlay, undo preview
// dialog (14.6), protected two-step reset (14.7), and the opposing-press
// conflict banner (12.4).
#include "padel/ui/tokens.hpp"
#include "screens.hpp"

namespace padel::ui::internal {
namespace {

// Wide enough for "RESET MATCH" plus its icon on one line at heading size.
constexpr lv_coord_t kMenuCardWidth = 460;

LiveScreen* self(lv_event_t* e) { return static_cast<LiveScreen*>(lv_event_get_user_data(e)); }

void on_award_a(lv_event_t* e);
void on_award_b(lv_event_t* e);

// Touchscreen fallback (spec 15): the whole team panel is a tap target that
// awards a point, so scoring works even with the remotes off.
void build_team_panel(LiveScreen::TeamPanel& panel, lv_obj_t* parent, lv_color_t accent,
                      lv_event_cb_t award_handler, void* user_data) {
    panel.panel = make_panel(parent);
    lv_obj_add_flag(panel.panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel.panel, award_handler, LV_EVENT_CLICKED, user_data);
    lv_obj_set_flex_grow(panel.panel, 1);
    lv_obj_set_height(panel.panel, LV_PCT(100));
    lv_obj_set_style_border_side(panel.panel, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(panel.panel, 6, 0);
    lv_obj_set_style_border_color(panel.panel, accent, 0);
    // Tight padding so the score can own the panel.
    lv_obj_set_style_pad_all(panel.panel, tokens::kSpaceS, 0);
    lv_obj_set_style_pad_row(panel.panel, tokens::kSpaceXs, 0);
    lv_obj_set_flex_flow(panel.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel.panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Long names ellipsize instead of overflowing the panel (spec 18.6).
    // Keep chrome compact so the score can dominate the panel.
    panel.name = make_label(panel.panel, tokens::font_heading(), accent);
    lv_obj_set_width(panel.name, LV_PCT(100));
    lv_label_set_long_mode(panel.name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(panel.name, LV_TEXT_ALIGN_CENTER, 0);
    panel.players = make_label(panel.panel, tokens::font_small(), tokens::text_muted());
    lv_obj_set_width(panel.players, LV_PCT(100));
    lv_label_set_long_mode(panel.players, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(panel.players, LV_TEXT_ALIGN_CENTER, 0);

    // Score claims all leftover height; digits stay centered in the slot.
    lv_obj_t* score_slot = lv_obj_create(panel.panel);
    lv_obj_set_size(score_slot, LV_PCT(100), 10);  // grows via flex
    lv_obj_set_flex_grow(score_slot, 1);
    lv_obj_set_style_bg_opa(score_slot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(score_slot, 0, 0);
    lv_obj_set_style_pad_all(score_slot, 0, 0);
    lv_obj_clear_flag(score_slot, LV_OBJ_FLAG_SCROLLABLE);
    // Let touches fall through to the panel's tap-to-score handler.
    lv_obj_clear_flag(score_slot, LV_OBJ_FLAG_CLICKABLE);

    panel.points = make_label(score_slot, tokens::font_score(), tokens::text());
    // "AD" is the widest score string — slight tracking so it fits the panel.
    lv_obj_set_style_text_letter_space(panel.points, -4, 0);
    lv_obj_center(panel.points);

    panel.plus_one = make_label(score_slot, tokens::font_large(), tokens::success());
    lv_label_set_text(panel.plus_one, "+1");
    lv_obj_align(panel.plus_one, LV_ALIGN_TOP_RIGHT, -tokens::kSpaceS, tokens::kSpaceS);
    lv_obj_add_flag(panel.plus_one, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* tag_row = lv_obj_create(panel.panel);
    lv_obj_set_size(tag_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(tag_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tag_row, 0, 0);
    lv_obj_set_style_pad_all(tag_row, 0, 0);
    lv_obj_set_flex_flow(tag_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tag_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tag_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tag_row, LV_OBJ_FLAG_CLICKABLE);

    panel.serving_tag = make_label(tag_row, tokens::font_body(), accent);
    lv_label_set_text(panel.serving_tag, LV_SYMBOL_PLAY " SERVING");
    panel.remote_tag = make_label(tag_row, tokens::font_small(), tokens::text_muted());
}

void update_team_panel(LiveScreen::TeamPanel& panel, const TeamPanelModel& m,
                       bool flash) {
    set_text(panel.name, m.name);
    set_text(panel.players, m.players);
    // Hide the players line when empty so the score can use the vertical space.
    if (m.players.empty()) {
        lv_obj_add_flag(panel.players, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(panel.players, LV_OBJ_FLAG_HIDDEN);
    }
    set_text(panel.points, m.points);

    if (m.serving) {
        lv_obj_clear_flag(panel.serving_tag, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(panel.serving_tag, LV_OBJ_FLAG_HIDDEN);
    }

    if (!m.remote_assigned) {
        set_text(panel.remote_tag, "NO REMOTE");
        lv_obj_set_style_text_color(panel.remote_tag, tokens::text_muted(), 0);
    } else if (m.remote_ok) {
        set_text(panel.remote_tag, "REMOTE OK");
        lv_obj_set_style_text_color(panel.remote_tag, tokens::success(), 0);
    } else {
        set_text(panel.remote_tag, "REMOTE ?");
        lv_obj_set_style_text_color(panel.remote_tag, tokens::warning(), 0);
    }

    if (flash) {
        lv_obj_clear_flag(panel.plus_one, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(panel.panel, tokens::surface_raised(), 0);
    } else {
        lv_obj_add_flag(panel.plus_one, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(panel.panel, tokens::surface(), 0);
    }
}

// --- Event handlers ----------------------------------------------------------

void on_menu(lv_event_t* e) { self(e)->open_organizer_menu(); }
void on_menu_close(lv_event_t* e) { self(e)->close_organizer_menu(); }

void on_award_a(lv_event_t* e) {
    LiveScreen* s = self(e);
    s->close_organizer_menu();
    if (s->shared->callbacks.award_point) s->shared->callbacks.award_point(TeamId::A);
}

void on_award_b(lv_event_t* e) {
    LiveScreen* s = self(e);
    s->close_organizer_menu();
    if (s->shared->callbacks.award_point) s->shared->callbacks.award_point(TeamId::B);
}

void on_undo(lv_event_t* e) {
    LiveScreen* s = self(e);
    s->close_organizer_menu();
    s->open_undo_dialog();
}

void on_pause(lv_event_t* e) {
    LiveScreen* s = self(e);
    s->close_organizer_menu();
    if (s->shared->callbacks.toggle_pause) s->shared->callbacks.toggle_pause();
}

void on_reset(lv_event_t* e) {
    LiveScreen* s = self(e);
    s->close_organizer_menu();
    s->open_reset_dialog(1);
}

void on_diagnostics(lv_event_t* e) {
    LiveScreen* s = self(e);
    s->close_organizer_menu();
    if (s->shared->callbacks.show_screen) s->shared->callbacks.show_screen(Screen::Diagnostics);
}

void on_conflict_a(lv_event_t* e) {
    if (self(e)->shared->callbacks.resolve_conflict)
        self(e)->shared->callbacks.resolve_conflict(TeamId::A);
}

void on_conflict_b(lv_event_t* e) {
    if (self(e)->shared->callbacks.resolve_conflict)
        self(e)->shared->callbacks.resolve_conflict(TeamId::B);
}

void on_conflict_cancel(lv_event_t* e) {
    if (self(e)->shared->callbacks.resolve_conflict)
        self(e)->shared->callbacks.resolve_conflict(std::nullopt);
}

void on_undo_dialog(lv_event_t* e) {
    LiveScreen* s = self(e);
    lv_obj_t* box = lv_event_get_current_target(e);
    const char* button = lv_msgbox_get_active_btn_text(box);
    if (button != nullptr && std::string(button) == "Undo" && s->shared->callbacks.undo_confirmed) {
        s->shared->callbacks.undo_confirmed();
    }
    lv_msgbox_close(box);
    s->undo_dialog = nullptr;
}

void on_reset_dialog1(lv_event_t* e) {
    LiveScreen* s = self(e);
    lv_obj_t* box = lv_event_get_current_target(e);
    const char* button = lv_msgbox_get_active_btn_text(box);
    const bool proceed = button != nullptr && std::string(button) == "Continue";
    lv_msgbox_close(box);
    s->reset_dialog1 = nullptr;
    if (proceed) {
        s->open_reset_dialog(2);
    }
}

void on_reset_dialog2(lv_event_t* e) {
    LiveScreen* s = self(e);
    lv_obj_t* box = lv_event_get_current_target(e);
    const char* button = lv_msgbox_get_active_btn_text(box);
    if (button != nullptr && std::string(button) == "RESET" && s->shared->callbacks.reset_confirmed) {
        s->shared->callbacks.reset_confirmed();
    }
    lv_msgbox_close(box);
    s->reset_dialog2 = nullptr;
}

}  // namespace

void LiveScreen::create(Shared* shared_state) {
    shared = shared_state;
    root = make_screen_root();
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, tokens::kSpaceS, 0);

    // --- Header ---------------------------------------------------------
    lv_obj_t* header = lv_obj_create(root);
    lv_obj_set_size(header, LV_PCT(100), 44);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    court_label = make_label(header, tokens::font_heading(), tokens::text());
    mode_label = make_label(header, tokens::font_body(), tokens::text_muted());
    status_label = make_label(header, tokens::font_heading(), tokens::success());
    storage_label = make_label(header, tokens::font_body(), tokens::error());
    lv_label_set_text(storage_label, LV_SYMBOL_WARNING " STORAGE");
    lv_obj_add_flag(storage_label, LV_OBJ_FLAG_HIDDEN);
    radio_label = make_label(header, tokens::font_body(), tokens::success());

    // --- Conflict banner (hidden unless a conflict is pending). Floating
    // overlay so showing it never reflows or shrinks the score panels. ----
    conflict_banner = lv_obj_create(root);
    lv_obj_add_flag(conflict_banner, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(conflict_banner, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_size(conflict_banner, LV_PCT(100), 76);
    lv_obj_set_style_bg_color(conflict_banner, tokens::error(), 0);
    lv_obj_set_style_radius(conflict_banner, tokens::kRadius, 0);
    lv_obj_set_style_border_width(conflict_banner, 0, 0);
    lv_obj_set_flex_flow(conflict_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(conflict_banner, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(conflict_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(conflict_banner, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* conflict_text = make_label(conflict_banner, tokens::font_heading(), tokens::text());
    lv_label_set_text(conflict_text, "BOTH TEAMS PRESSED - SELECT WINNER");
    conflict_btn_a = make_button(conflict_banner, "TEAM A", tokens::kTouchTarget, on_conflict_a, this);
    conflict_btn_b = make_button(conflict_banner, "TEAM B", tokens::kTouchTarget, on_conflict_b, this);
    make_button(conflict_banner, "CANCEL", tokens::kTouchTarget, on_conflict_cancel, this);

    // --- Team panels -------------------------------------------------------
    lv_obj_t* center = lv_obj_create(root);
    lv_obj_set_size(center, LV_PCT(100), 10);  // grows via flex
    lv_obj_set_flex_grow(center, 1);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(center, 0, 0);
    lv_obj_set_style_pad_all(center, 0, 0);
    lv_obj_set_style_pad_column(center, tokens::kSpaceM, 0);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(center, LV_OBJ_FLAG_SCROLLABLE);

    build_team_panel(team_a, center, tokens::team_a(), on_award_a, this);
    build_team_panel(team_b, center, tokens::team_b(), on_award_b, this);

    // --- Footer: broadcast-style scoreboard + organizer entry ---------------
    lv_obj_t* footer = lv_obj_create(root);
    lv_obj_set_size(footer, LV_PCT(100), 132);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, 0, 0);
    lv_obj_set_style_pad_column(footer, tokens::kSpaceM, 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    scoreboard.create(footer, 60, tokens::font_large(), tokens::font_banner());

    lv_obj_t* footer_right = lv_obj_create(footer);
    lv_obj_set_size(footer_right, 220, LV_PCT(100));
    lv_obj_set_style_bg_opa(footer_right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer_right, 0, 0);
    lv_obj_set_style_pad_all(footer_right, 0, 0);
    lv_obj_set_style_pad_row(footer_right, tokens::kSpaceXs, 0);
    lv_obj_set_flex_flow(footer_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(footer_right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_clear_flag(footer_right, LV_OBJ_FLAG_SCROLLABLE);

    special_label = make_label(footer_right, tokens::font_heading(), tokens::warning());
    menu_button = make_button(footer_right, LV_SYMBOL_SETTINGS " MENU", tokens::kOrganizerTarget,
                              on_menu, this);

    // --- Organizer overlay (hidden) ----------------------------------------
    // Floating so opening the menu never reflows the score panels behind it,
    // and full-screen so the dimmed backdrop swallows the taps that would
    // otherwise land on a team panel and score a point. Tapping the backdrop
    // dismisses the menu.
    organizer_overlay = lv_obj_create(root);
    lv_obj_add_flag(organizer_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(organizer_overlay, tokens::kScreenWidth, tokens::kScreenHeight);
    lv_obj_align(organizer_overlay, LV_ALIGN_TOP_LEFT, -tokens::kSpaceM, -tokens::kSpaceM);
    lv_obj_set_style_bg_color(organizer_overlay, tokens::bg(), 0);
    lv_obj_set_style_bg_opa(organizer_overlay, LV_OPA_70, 0);
    lv_obj_set_style_radius(organizer_overlay, 0, 0);
    lv_obj_set_style_border_width(organizer_overlay, 0, 0);
    lv_obj_set_style_pad_all(organizer_overlay, 0, 0);
    lv_obj_add_flag(organizer_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(organizer_overlay, on_menu_close, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(organizer_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(organizer_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = lv_obj_create(organizer_overlay);
    lv_obj_set_size(card, kMenuCardWidth, LV_SIZE_CONTENT);
    lv_obj_center(card);
    // Clickable so a miss between rows is absorbed instead of reaching the
    // backdrop's dismiss handler.
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(card, tokens::surface(), 0);
    lv_obj_set_style_radius(card, tokens::kRadius, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, tokens::surface_raised(), 0);
    lv_obj_set_style_shadow_width(card, 48, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_pad_all(card, tokens::kSpaceL, 0);
    lv_obj_set_style_pad_row(card, tokens::kSpaceS, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = make_label(card, tokens::font_body(), tokens::text_muted());
    lv_label_set_text(title, "ORGANIZER");
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_style_pad_bottom(title, tokens::kSpaceXs, 0);

    // Menu rows read as a list: icon and text left-aligned on a common margin
    // rather than centered per row.
    const auto menu_row = [&](const char* text, lv_event_cb_t handler, lv_color_t color) {
        lv_obj_t* b = make_button(card, text, tokens::kOrganizerTarget, handler, this);
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_t* label = lv_obj_get_child(b, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_text_color(label, color, 0);
        return b;
    };
    // +1 buttons removed: tapping a team's score panel awards the point.
    menu_row(LV_SYMBOL_LEFT " UNDO", on_undo, tokens::text());
    lv_obj_t* pause_button = menu_row("PAUSE", on_pause, tokens::text());
    pause_button_label = lv_obj_get_child(pause_button, 0);
    menu_row(LV_SYMBOL_LIST " DIAGNOSTICS", on_diagnostics, tokens::text());
    menu_row(LV_SYMBOL_TRASH " RESET MATCH", on_reset, tokens::error());

    // Rule detaches the dismiss row from the actions above it.
    lv_obj_t* divider = lv_obj_create(card);
    lv_obj_set_size(divider, LV_PCT(100), 2);
    lv_obj_set_style_bg_color(divider, tokens::text_muted(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_40, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* close_button = menu_row("CLOSE", on_menu_close, tokens::text_muted());
    lv_obj_set_style_bg_opa(close_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(close_button, 2, 0);
    lv_obj_set_style_border_color(close_button, tokens::surface_raised(), 0);
    lv_obj_align(lv_obj_get_child(close_button, 0), LV_ALIGN_CENTER, 0, 0);
}

void LiveScreen::open_organizer_menu() {
    lv_obj_clear_flag(organizer_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(organizer_overlay);
}

void LiveScreen::close_organizer_menu() {
    lv_obj_add_flag(organizer_overlay, LV_OBJ_FLAG_HIDDEN);
}

void LiveScreen::open_undo_dialog() {
    static const char* buttons[] = {"Undo", "Cancel", ""};
    undo_dialog = lv_msgbox_create(nullptr, "Undo", undo_preview_text.c_str(), buttons, false);
    lv_obj_add_event_cb(undo_dialog, on_undo_dialog, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_center(undo_dialog);
}

void LiveScreen::open_reset_dialog(int step) {
    if (step == 1) {
        static const char* buttons[] = {"Continue", "Cancel", ""};
        reset_dialog1 = lv_msgbox_create(nullptr, "Reset match?",
                                         "The current match will be archived.", buttons, false);
        lv_obj_add_event_cb(reset_dialog1, on_reset_dialog1, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_center(reset_dialog1);
    } else {
        static const char* buttons[] = {"RESET", "Cancel", ""};
        reset_dialog2 = lv_msgbox_create(nullptr, "Confirm reset",
                                         "Really reset? This cannot be undone.", buttons, false);
        lv_obj_add_event_cb(reset_dialog2, on_reset_dialog2, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_center(reset_dialog2);
    }
}

void LiveScreen::close_dialogs() {
    if (undo_dialog != nullptr) {
        lv_msgbox_close(undo_dialog);
        undo_dialog = nullptr;
    }
    if (reset_dialog1 != nullptr) {
        lv_msgbox_close(reset_dialog1);
        reset_dialog1 = nullptr;
    }
    if (reset_dialog2 != nullptr) {
        lv_msgbox_close(reset_dialog2);
        reset_dialog2 = nullptr;
    }
}

void LiveScreen::update(const LiveViewModel& m) {
    set_text(court_label, m.court_label);
    set_text(mode_label, m.mode_label);
    set_text(status_label, m.status_label);
    lv_obj_set_style_text_color(status_label, m.paused ? tokens::warning() : tokens::success(), 0);

    set_text(radio_label, m.radio_ok ? "RADIO: OK" : "RADIO: CHECK");
    lv_obj_set_style_text_color(radio_label, m.radio_ok ? tokens::success() : tokens::warning(), 0);

    if (m.storage_fault) {
        lv_obj_clear_flag(storage_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(storage_label, LV_OBJ_FLAG_HIDDEN);
    }

    update_team_panel(team_a, m.team_a, m.point_flash == TeamId::A);
    update_team_panel(team_b, m.team_b, m.point_flash == TeamId::B);

    set_text(special_label, m.special_label);
    scoreboard.update(m.scoreboard);

    if (m.conflict) {
        lv_obj_clear_flag(conflict_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(conflict_banner);
    } else {
        lv_obj_add_flag(conflict_banner, LV_OBJ_FLAG_HIDDEN);
    }

    if (m.undo_preview) {
        undo_preview_text = std::string("Undo ") +
                            (*m.undo_preview == TeamId::A ? m.team_a.name : m.team_b.name) +
                            " point?";
    } else {
        undo_preview_text = "Nothing to undo.";
    }
    // Update the pause button label to reflect the next action.
    if (pause_button_label != nullptr) {
        set_text(pause_button_label, m.paused ? "RESUME" : "PAUSE");
    }
}

}  // namespace padel::ui::internal
