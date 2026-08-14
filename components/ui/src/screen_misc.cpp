// Match complete (spec 14.8), pairing (14.5), diagnostics (14.9), and boot
// recovery (12.2) screens.
#include "padel/ui/tokens.hpp"
#include "screens.hpp"

namespace padel::ui::internal {
namespace {

template <typename ScreenT>
ScreenT* self(lv_event_t* e) {
    return static_cast<ScreenT*>(lv_event_get_user_data(e));
}

lv_obj_t* centered_column(lv_obj_t* parent) {
    lv_obj_t* column = lv_obj_create(parent);
    lv_obj_set_size(column, LV_PCT(70), LV_SIZE_CONTENT);
    lv_obj_center(column);
    lv_obj_set_style_bg_color(column, tokens::surface(), 0);
    lv_obj_set_style_radius(column, tokens::kRadius, 0);
    lv_obj_set_style_border_width(column, 0, 0);
    lv_obj_set_style_pad_all(column, tokens::kSpaceXl, 0);
    lv_obj_set_style_pad_row(column, tokens::kSpaceM, 0);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    return column;
}

lv_obj_t* button_row(lv_obj_t* parent) {
    lv_obj_t* buttons = lv_obj_create(parent);
    lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(buttons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(buttons, 0, 0);
    lv_obj_set_style_pad_all(buttons, 0, 0);
    lv_obj_set_style_pad_column(buttons, tokens::kSpaceM, 0);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);
    return buttons;
}

}  // namespace

// --- Match summary --------------------------------------------------------------

void SummaryScreen::create(Shared* shared_state) {
    shared = shared_state;
    root = make_screen_root();
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    // The densest screen in the UI: a five-set board, five stat rows and two
    // buttons only fit between the banner and the bottom edge on tight gaps.
    lv_obj_set_style_pad_row(root, tokens::kSpaceS, 0);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    title_label = make_label(root, tokens::font_large(), tokens::text_muted());
    winner_label = make_label(root, tokens::font_banner(), tokens::success());
    lv_label_set_long_mode(winner_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(winner_label, LV_PCT(100));
    lv_obj_set_style_text_align(winner_label, LV_TEXT_ALIGN_CENTER, 0);

    scoreboard.create(root, 48, tokens::font_large(), tokens::font_large());

    lv_obj_t* stats_panel = make_panel(root);
    lv_obj_set_size(stats_panel, 640, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(stats_panel, tokens::kSpaceM, 0);
    lv_obj_set_style_pad_row(stats_panel, tokens::kSpaceXs, 0);
    lv_obj_set_flex_flow(stats_panel, LV_FLEX_FLOW_COLUMN);

    for (StatRow& stat : stats) {
        stat.panel = lv_obj_create(stats_panel);
        lv_obj_set_size(stat.panel, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(stat.panel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(stat.panel, 0, 0);
        lv_obj_set_style_pad_all(stat.panel, 0, 0);
        lv_obj_set_flex_flow(stat.panel, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(stat.panel, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(stat.panel, LV_OBJ_FLAG_SCROLLABLE);

        stat.label = make_label(stat.panel, tokens::font_heading(), tokens::text_muted());
        lv_label_set_long_mode(stat.label, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(stat.label, 1);
        stat.value = make_label(stat.panel, tokens::font_heading(), tokens::text());
    }

    lv_obj_t* buttons = button_row(root);
    add_back_undo_button(buttons, this);
    lv_obj_t* button = make_button(
        buttons, "CONTINUE", tokens::kOrganizerTarget,
        [](lv_event_t* e) {
            auto* s = self<SummaryScreen>(e);
            if (s->shared->callbacks.summary_continue) s->shared->callbacks.summary_continue();
        },
        this);
    lv_obj_set_style_bg_color(button, tokens::success(), 0);
    continue_label = lv_obj_get_child(button, 0);
}

void SummaryScreen::update(const SummaryViewModel& model) {
    set_text(title_label, model.title);
    set_text(winner_label, model.winner_label);
    set_text(continue_label, model.continue_label);
    scoreboard.update(model.scoreboard, model.prior_scoreboards);

    for (std::size_t i = 0; i < kMaxStatRows; ++i) {
        StatRow& stat = stats[i];
        if (i >= model.stats.size()) {
            lv_obj_add_flag(stat.panel, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(stat.panel, LV_OBJ_FLAG_HIDDEN);
        set_text(stat.label, model.stats[i].first);
        set_text(stat.value, model.stats[i].second);
    }
}

// --- Match complete ----------------------------------------------------------

void CompleteScreen::create(Shared* shared_state) {
    shared = shared_state;
    root = make_screen_root();

    lv_obj_t* column = centered_column(root);

    winner_label = make_label(column, tokens::font_banner(), tokens::success());
    lv_label_set_long_mode(winner_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(winner_label, LV_PCT(100));
    lv_obj_set_style_text_align(winner_label, LV_TEXT_ALIGN_CENTER, 0);
    score_label = make_label(column, tokens::font_large(), tokens::text());
    duration_label = make_label(column, tokens::font_body(), tokens::text_muted());

    // Spec 14.8 also lists a "Review / correct" button here, which only moved
    // to the live screen and left the score alone. BACK reaches the same screen
    // and is the reason anyone went there; reviewing is what the summary before
    // this one is for (ADR-0022).
    lv_obj_t* buttons = button_row(column);
    add_back_undo_button(buttons, this);
    lv_obj_t* next = make_button(
        buttons, "NEW MATCH", tokens::kOrganizerTarget,
        [](lv_event_t* e) {
            auto* s = self<CompleteScreen>(e);
            if (s->shared->callbacks.new_match) s->shared->callbacks.new_match();
        },
        this);
    lv_obj_set_style_bg_color(next, tokens::success(), 0);

    lv_obj_t* future = make_label(column, tokens::font_small(), tokens::text_muted());
    lv_label_set_text(future, "Next assignments will appear here (multi-court, future)");
}

void CompleteScreen::update(const CompleteViewModel& model) {
    set_text(winner_label, model.winner_label);
    set_text(score_label, model.final_score);
    set_text(duration_label, model.duration_label);
}

// --- Pairing -----------------------------------------------------------------

void PairingScreen::create(Shared* shared_state) {
    shared = shared_state;
    root = make_screen_root();

    lv_obj_t* column = centered_column(root);

    team_label = make_label(column, tokens::font_large(), tokens::text());
    instruction_label = make_label(column, tokens::font_body(), tokens::text_muted());
    lv_label_set_long_mode(instruction_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(instruction_label, LV_PCT(100));
    candidate_label = make_label(column, tokens::font_large(), tokens::warning());
    countdown_label = make_label(column, tokens::font_body(), tokens::text_muted());

    lv_obj_t* buttons = button_row(column);
    confirm_button = make_button(
        buttons, "CONFIRM", tokens::kOrganizerTarget,
        [](lv_event_t* e) {
            auto* s = self<PairingScreen>(e);
            if (s->shared->callbacks.confirm_pairing) s->shared->callbacks.confirm_pairing();
        },
        this);
    lv_obj_set_style_bg_color(confirm_button, tokens::success(), 0);
    make_button(
        buttons, "CANCEL", tokens::kOrganizerTarget,
        [](lv_event_t* e) {
            auto* s = self<PairingScreen>(e);
            if (s->shared->callbacks.cancel_pairing) s->shared->callbacks.cancel_pairing();
        },
        this);
}

void PairingScreen::update(const PairingViewModel& model) {
    set_text(team_label, model.team_label);
    set_text(instruction_label, model.instruction);
    set_text(candidate_label, model.candidate_label);
    set_text(countdown_label,
             model.seconds_left > 0 ? std::to_string(model.seconds_left) + " s remaining" : "");
    if (model.awaiting_confirm) {
        lv_obj_clear_flag(confirm_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(confirm_button, LV_OBJ_FLAG_HIDDEN);
    }
}

// --- Diagnostics --------------------------------------------------------------

void DiagnosticsScreen::create(Shared* shared_state) {
    shared = shared_state;
    root = make_screen_root();
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, tokens::kSpaceS, 0);

    lv_obj_t* header = lv_obj_create(root);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = make_label(header, tokens::font_large(), tokens::text());
    lv_label_set_text(title, "DIAGNOSTICS");
    make_button(
        header, "BEEP TEST", tokens::kTouchTarget,
        [](lv_event_t* e) {
            auto* s = self<DiagnosticsScreen>(e);
            if (s->shared->callbacks.test_beep) s->shared->callbacks.test_beep();
        },
        this);
    make_button(
        header, "BACK", tokens::kTouchTarget,
        [](lv_event_t* e) {
            auto* s = self<DiagnosticsScreen>(e);
            if (s->shared->callbacks.show_screen) s->shared->callbacks.show_screen(Screen::Setup);
        },
        this);

    lv_obj_t* body = lv_obj_create(root);
    lv_obj_set_size(body, LV_PCT(100), 10);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_column(body, tokens::kSpaceM, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    table = lv_table_create(body);
    lv_obj_set_size(table, LV_PCT(55), LV_PCT(100));
    lv_obj_set_style_bg_color(table, tokens::surface(), 0);
    lv_obj_set_style_text_font(table, tokens::font_small(), 0);
    lv_obj_set_style_bg_color(table, tokens::surface(), LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, tokens::text(), LV_PART_ITEMS);
    lv_obj_set_style_border_color(table, tokens::surface_raised(), LV_PART_ITEMS);
    lv_table_set_col_width(table, 0, 240);
    lv_table_set_col_width(table, 1, 300);

    lv_obj_t* log_panel = make_panel(body);
    lv_obj_set_size(log_panel, LV_PCT(43), LV_PCT(100));
    lv_obj_add_flag(log_panel, LV_OBJ_FLAG_SCROLLABLE);
    log_label = make_label(log_panel, tokens::font_small(), tokens::text_muted());
    lv_label_set_long_mode(log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(log_label, LV_PCT(100));
}

void DiagnosticsScreen::update(const DiagnosticsViewModel& model) {
    lv_table_set_row_cnt(table, static_cast<uint16_t>(model.rows.size()));
    lv_table_set_col_cnt(table, 2);
    for (std::size_t i = 0; i < model.rows.size(); ++i) {
        lv_table_set_cell_value(table, static_cast<uint16_t>(i), 0, model.rows[i].first.c_str());
        lv_table_set_cell_value(table, static_cast<uint16_t>(i), 1, model.rows[i].second.c_str());
    }

    std::string log_text;
    for (const std::string& line : model.recent_log_lines) {
        log_text += line;
        log_text += "\n";
    }
    set_text(log_label, log_text);
}

// --- Recovery ------------------------------------------------------------------

void RecoveryScreen::create(Shared* shared_state) {
    shared = shared_state;
    root = make_screen_root();

    lv_obj_t* column = centered_column(root);

    lv_obj_t* title = make_label(column, tokens::font_large(), tokens::warning());
    lv_label_set_text(title, LV_SYMBOL_REFRESH " MATCH RECOVERY");

    message_label = make_label(column, tokens::font_heading(), tokens::text());
    lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(message_label, LV_PCT(100));
    detail_label = make_label(column, tokens::font_body(), tokens::text_muted());
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detail_label, LV_PCT(100));

    lv_obj_t* buttons = button_row(column);
    lv_obj_t* resume = make_button(
        buttons, "RESUME MATCH", tokens::kOrganizerTarget,
        [](lv_event_t* e) {
            auto* s = self<RecoveryScreen>(e);
            if (s->shared->callbacks.recovery_choice) s->shared->callbacks.recovery_choice(true);
        },
        this);
    lv_obj_set_style_bg_color(resume, tokens::success(), 0);
    make_button(
        buttons, "DISCARD", tokens::kOrganizerTarget,
        [](lv_event_t* e) {
            auto* s = self<RecoveryScreen>(e);
            if (s->shared->callbacks.recovery_choice) s->shared->callbacks.recovery_choice(false);
        },
        this);
}

void RecoveryScreen::update(const RecoveryViewModel& model) {
    set_text(message_label, model.message);
    set_text(detail_label, model.detail);
}

}  // namespace padel::ui::internal
