#include <algorithm>
#include <cstring>

#include "padel/ui/tokens.hpp"
#include "screens.hpp"

namespace padel::ui::internal {

void set_text(lv_obj_t* label, const std::string& text) {
    if (label != nullptr && text != lv_label_get_text(label)) {
        lv_label_set_text(label, text.c_str());
    }
}

void set_text(lv_obj_t* label, const char* text) {
    if (label != nullptr && std::strcmp(text, lv_label_get_text(label)) != 0) {
        lv_label_set_text(label, text);
    }
}

lv_obj_t* make_screen_root() {
    lv_obj_t* root = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(root, tokens::bg(), 0);
    lv_obj_set_style_text_color(root, tokens::text(), 0);
    lv_obj_set_style_pad_all(root, tokens::kSpaceM, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    return root;
}

lv_obj_t* make_panel(lv_obj_t* parent) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_style_bg_color(panel, tokens::surface(), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, tokens::kRadius, 0);
    lv_obj_set_style_pad_all(panel, tokens::kSpaceM, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, "");
    return label;
}

lv_obj_t* make_button(lv_obj_t* parent, const char* text, lv_coord_t min_height,
                      lv_event_cb_t handler, void* user_data) {
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_style_bg_color(button, tokens::surface_raised(), 0);
    lv_obj_set_style_radius(button, tokens::kRadius, 0);
    lv_obj_set_height(button, min_height);
    lv_obj_set_style_pad_hor(button, tokens::kSpaceL, 0);
    if (handler != nullptr) {
        lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t* label = lv_label_create(button);
    lv_obj_set_style_text_font(label, tokens::font_heading(), 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

// --- Modal confirmation dialog -------------------------------------------------

namespace {

// Two 80 px buttons side by side plus the message need this much card.
constexpr lv_coord_t kDialogWidth = 620;

}  // namespace

Dialog make_dialog(lv_obj_t* parent, const char* title, const char* message,
                   lv_event_cb_t on_dismiss, void* user_data) {
    Dialog dialog{};

    dialog.overlay = lv_obj_create(parent);
    lv_obj_add_flag(dialog.overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(dialog.overlay, tokens::kScreenWidth, tokens::kScreenHeight);
    // Pull back over the screen root's padding so the backdrop reaches the edge.
    lv_obj_align(dialog.overlay, LV_ALIGN_TOP_LEFT,
                 -lv_obj_get_style_pad_left(parent, 0), -lv_obj_get_style_pad_top(parent, 0));
    lv_obj_set_style_bg_color(dialog.overlay, tokens::bg(), 0);
    lv_obj_set_style_bg_opa(dialog.overlay, LV_OPA_80, 0);
    lv_obj_set_style_radius(dialog.overlay, 0, 0);
    lv_obj_set_style_border_width(dialog.overlay, 0, 0);
    lv_obj_set_style_pad_all(dialog.overlay, 0, 0);
    lv_obj_add_flag(dialog.overlay, LV_OBJ_FLAG_CLICKABLE);
    if (on_dismiss != nullptr) {
        lv_obj_add_event_cb(dialog.overlay, on_dismiss, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_clear_flag(dialog.overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = lv_obj_create(dialog.overlay);
    lv_obj_set_size(card, kDialogWidth, LV_SIZE_CONTENT);
    lv_obj_center(card);
    // Clickable so a miss between the buttons is absorbed instead of reaching
    // the backdrop's dismiss handler.
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(card, tokens::surface(), 0);
    lv_obj_set_style_radius(card, tokens::kRadius, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, tokens::surface_raised(), 0);
    lv_obj_set_style_shadow_width(card, 48, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_pad_all(card, tokens::kSpaceXl, 0);
    lv_obj_set_style_pad_row(card, tokens::kSpaceM, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title_label = make_label(card, tokens::font_large(), tokens::text());
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_letter_space(title_label, 2, 0);

    lv_obj_t* message_label = make_label(card, tokens::font_heading(), tokens::text_muted());
    lv_obj_set_width(message_label, LV_PCT(100));
    lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(message_label, message);

    dialog.buttons = lv_obj_create(card);
    lv_obj_set_size(dialog.buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(dialog.buttons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dialog.buttons, 0, 0);
    lv_obj_set_style_pad_all(dialog.buttons, 0, 0);
    lv_obj_set_style_pad_top(dialog.buttons, tokens::kSpaceS, 0);
    lv_obj_set_style_pad_column(dialog.buttons, tokens::kSpaceM, 0);
    lv_obj_set_flex_flow(dialog.buttons, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(dialog.buttons, LV_OBJ_FLAG_SCROLLABLE);

    return dialog;
}

lv_obj_t* add_dialog_button(const Dialog& dialog, const char* text, lv_color_t color,
                            lv_event_cb_t handler, void* user_data) {
    lv_obj_t* button =
        make_button(dialog.buttons, text, tokens::kDialogTarget, handler, user_data);
    // Equal shares of the row: both actions end up the same wide target.
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(button, 0), tokens::font_large(), 0);
    return button;
}

// --- Broadcast-style scoreboard --------------------------------------------

namespace {

// Pair labels are cut to three capitals a side ("JOS/RUX"), which is what
// lets two blocks of the strip share the footer with the MENU column.
constexpr lv_coord_t kPlateWidth = 240;
constexpr lv_coord_t kCellWidth = 76;
constexpr lv_coord_t kServeDotSize = 16;

void build_scoreboard_row(ScoreboardWidget::Row& row, lv_obj_t* parent, lv_color_t accent,
                          lv_coord_t height, const lv_font_t* name_font,
                          const lv_font_t* digit_font) {
    row.panel = lv_obj_create(parent);
    lv_obj_set_size(row.panel, LV_SIZE_CONTENT, height);
    lv_obj_set_style_bg_opa(row.panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row.panel, 0, 0);
    lv_obj_set_style_pad_all(row.panel, 0, 0);
    lv_obj_set_style_pad_column(row.panel, tokens::kSpaceXs, 0);
    lv_obj_set_flex_flow(row.panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row.panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row.panel, LV_OBJ_FLAG_SCROLLABLE);

    row.plate = lv_obj_create(row.panel);
    lv_obj_set_size(row.plate, kPlateWidth, height);
    lv_obj_set_style_bg_color(row.plate, accent, 0);
    lv_obj_set_style_border_width(row.plate, 0, 0);
    lv_obj_set_style_radius(row.plate, tokens::kRadius, 0);
    lv_obj_set_style_pad_hor(row.plate, tokens::kSpaceS, 0);
    lv_obj_set_style_pad_ver(row.plate, 0, 0);
    lv_obj_set_style_pad_column(row.plate, tokens::kSpaceS, 0);
    lv_obj_set_flex_flow(row.plate, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row.plate, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row.plate, LV_OBJ_FLAG_SCROLLABLE);

    // Serve indicator: the ball dot the broadcasts put next to the server.
    row.serve_dot = lv_obj_create(row.plate);
    lv_obj_set_size(row.serve_dot, kServeDotSize, kServeDotSize);
    lv_obj_set_style_radius(row.serve_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(row.serve_dot, tokens::warning(), 0);
    lv_obj_set_style_border_width(row.serve_dot, 0, 0);
    lv_obj_clear_flag(row.serve_dot, LV_OBJ_FLAG_SCROLLABLE);

    row.name = make_label(row.plate, name_font, tokens::text());
    lv_obj_set_flex_grow(row.name, 1);
    lv_label_set_long_mode(row.name, LV_LABEL_LONG_DOT);

    for (int i = 0; i < ScoreboardWidget::kMaxColumns; ++i) {
        lv_obj_t* cell = lv_obj_create(row.panel);
        lv_obj_set_size(cell, kCellWidth, height);
        lv_obj_set_style_bg_color(cell, tokens::surface(), 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_radius(cell, tokens::kRadius, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* digit = make_label(cell, digit_font, tokens::text());
        lv_obj_align(digit, LV_ALIGN_CENTER, -tokens::kSpaceS, 0);

        lv_obj_t* tiebreak = make_label(cell, tokens::font_small(), tokens::text_muted());
        lv_obj_align(tiebreak, LV_ALIGN_TOP_RIGHT, -tokens::kSpaceXs, tokens::kSpaceXs);

        row.cells[i] = cell;
        row.digits[i] = digit;
        row.tiebreaks[i] = tiebreak;
    }
}

void update_scoreboard_row(ScoreboardWidget::Row& row, const ScoreboardModel& model,
                           TeamId team) {
    set_text(row.name, team == TeamId::A ? model.name_a : model.name_b);
    if (model.serving && *model.serving == team) {
        lv_obj_clear_flag(row.serve_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(row.serve_dot, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < ScoreboardWidget::kMaxColumns; ++i) {
        if (static_cast<std::size_t>(i) >= model.columns.size()) {
            lv_obj_add_flag(row.cells[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const ScoreColumn& column = model.columns[static_cast<std::size_t>(i)];
        lv_obj_clear_flag(row.cells[i], LV_OBJ_FLAG_HIDDEN);
        set_text(row.digits[i], team == TeamId::A ? column.games_a : column.games_b);
        set_text(row.tiebreaks[i], team == TeamId::A ? column.tiebreak_a : column.tiebreak_b);

        // The set in progress is the one the crowd is watching, so it gets the
        // lit cell; a set the team lost fades back.
        const bool lost = column.won.has_value() && *column.won != team;
        lv_obj_set_style_bg_color(row.cells[i],
                                  column.current ? tokens::surface_raised() : tokens::surface(),
                                  0);
        lv_obj_set_style_text_color(row.digits[i],
                                    lost ? tokens::text_muted() : tokens::text(), 0);
    }
}

}  // namespace

void ScoreboardWidget::create(lv_obj_t* parent, lv_coord_t row_height,
                              const lv_font_t* name_font, const lv_font_t* digit_font) {
    root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_row(root, tokens::kSpaceXs, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    build_scoreboard_row(row_a, root, tokens::team_a(), row_height, name_font, digit_font);
    build_scoreboard_row(row_b, root, tokens::team_b(), row_height, name_font, digit_font);
}

void ScoreboardWidget::update(const ScoreboardModel& model) {
    update_scoreboard_row(row_a, model, TeamId::A);
    update_scoreboard_row(row_b, model, TeamId::B);
}

void ScoreboardStrip::create(lv_obj_t* parent, lv_coord_t row_height,
                             const lv_font_t* name_font, const lv_font_t* digit_font) {
    root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_column(root, tokens::kSpaceM, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    for (ScoreboardWidget& block : blocks) {
        block.create(root, row_height, name_font, digit_font);
        lv_obj_add_flag(block.root, LV_OBJ_FLAG_HIDDEN);
    }
}

void ScoreboardStrip::update(const ScoreboardModel& current, const PriorScoreboards& prior) {
    // The board for the set on screen is always last, so it keeps its place on
    // the right however many sets came before it.
    const std::size_t shown = std::min<std::size_t>(prior.size(), kMaxBlocks - 1);
    for (std::size_t i = 0; i < shown; ++i) {
        lv_obj_clear_flag(blocks[i].root, LV_OBJ_FLAG_HIDDEN);
        blocks[i].update(prior[i]);
    }
    lv_obj_clear_flag(blocks[shown].root, LV_OBJ_FLAG_HIDDEN);
    blocks[shown].update(current);
    for (std::size_t i = shown + 1; i < kMaxBlocks; ++i) {
        lv_obj_add_flag(blocks[i].root, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace padel::ui::internal
