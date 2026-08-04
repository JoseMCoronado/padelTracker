// Setup screen (spec 14.4): court label, team/player names, scoring preset,
// first server, remote pairing status, start.
#include "padel/ui/tokens.hpp"
#include "screens.hpp"

namespace padel::ui::internal {
namespace {

SetupScreen* self(lv_event_t* e) { return static_cast<SetupScreen*>(lv_event_get_user_data(e)); }

lv_obj_t* make_field(lv_obj_t* parent, const char* title, SetupScreen* screen) {
    lv_obj_t* column = lv_obj_create(parent);
    lv_obj_set_size(column, LV_PCT(48), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(column, 0, 0);
    lv_obj_set_style_pad_all(column, 0, 0);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, tokens::kSpaceXs, 0);
    lv_obj_clear_flag(column, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* label = make_label(column, tokens::font_small(), tokens::text_muted());
    lv_label_set_text(label, title);

    lv_obj_t* field = lv_textarea_create(column);
    lv_textarea_set_one_line(field, true);
    lv_obj_set_width(field, LV_PCT(100));
    lv_obj_set_style_bg_color(field, tokens::surface_raised(), 0);
    lv_obj_set_style_text_font(field, tokens::font_heading(), 0);
    // Focus/defocus drives the shared on-screen keyboard.
    lv_obj_add_event_cb(
        field,
        [](lv_event_t* e) {
            SetupScreen* s = self(e);
            lv_obj_t* target = lv_event_get_target(e);
            if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
                lv_keyboard_set_textarea(s->keyboard, target);
                lv_obj_clear_flag(s->keyboard, LV_OBJ_FLAG_HIDDEN);
            } else if (lv_event_get_code(e) == LV_EVENT_DEFOCUSED) {
                lv_obj_add_flag(s->keyboard, LV_OBJ_FLAG_HIDDEN);
            }
        },
        LV_EVENT_ALL, screen);
    return field;
}

void on_start(lv_event_t* e) {
    SetupScreen* s = self(e);
    if (s->shared->callbacks.start_match) {
        s->shared->callbacks.start_match(s->read_settings());
    }
}

void on_pair_a(lv_event_t* e) {
    SetupScreen* s = self(e);
    if (s->shared->callbacks.begin_pairing) s->shared->callbacks.begin_pairing(TeamId::A);
}

void on_pair_b(lv_event_t* e) {
    SetupScreen* s = self(e);
    if (s->shared->callbacks.begin_pairing) s->shared->callbacks.begin_pairing(TeamId::B);
}

void on_diagnostics(lv_event_t* e) {
    SetupScreen* s = self(e);
    if (s->shared->callbacks.show_screen) s->shared->callbacks.show_screen(Screen::Diagnostics);
}

std::string dropdown_options(const std::vector<std::string>& names) {
    std::string options;
    for (const std::string& name : names) {
        if (!options.empty()) {
            options += "\n";
        }
        options += name;
    }
    return options;
}

}  // namespace

void SetupScreen::create(Shared* shared_state) {
    shared = shared_state;
    root = make_screen_root();
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, tokens::kSpaceS, 0);

    lv_obj_t* title = make_label(root, tokens::font_large(), tokens::text());
    lv_label_set_text(title, "MATCH SETUP");

    lv_obj_t* form = make_panel(root);
    lv_obj_set_size(form, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(form, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(form, tokens::kSpaceS, 0);

    court_field = make_field(form, "COURT LABEL", this);
    // Preset + server dropdowns share a column.
    lv_obj_t* rules_column = lv_obj_create(form);
    lv_obj_set_size(rules_column, LV_PCT(48), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(rules_column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rules_column, 0, 0);
    lv_obj_set_style_pad_all(rules_column, 0, 0);
    lv_obj_set_flex_flow(rules_column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(rules_column, tokens::kSpaceXs, 0);
    lv_obj_clear_flag(rules_column, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* preset_title = make_label(rules_column, tokens::font_small(), tokens::text_muted());
    lv_label_set_text(preset_title, "SCORING PRESET");
    preset_dropdown = lv_dropdown_create(rules_column);
    lv_obj_set_width(preset_dropdown, LV_PCT(100));
    lv_dropdown_set_options(preset_dropdown, dropdown_options(preset_names()).c_str());

    lv_obj_t* server_title = make_label(rules_column, tokens::font_small(), tokens::text_muted());
    lv_label_set_text(server_title, "FIRST SERVER");
    server_dropdown = lv_dropdown_create(rules_column);
    lv_obj_set_width(server_dropdown, LV_PCT(100));
    lv_dropdown_set_options(server_dropdown, "Team A\nTeam B");

    team_a_field = make_field(form, "TEAM A NAME", this);
    team_b_field = make_field(form, "TEAM B NAME", this);
    players_a_field = make_field(form, "TEAM A PLAYERS (OPTIONAL)", this);
    players_b_field = make_field(form, "TEAM B PLAYERS (OPTIONAL)", this);

    // Remote status + pairing entry points.
    lv_obj_t* remotes = make_panel(root);
    lv_obj_set_size(remotes, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(remotes, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(remotes, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    remote_a_status = make_label(remotes, tokens::font_body(), tokens::text());
    make_button(remotes, "PAIR TEAM A", tokens::kTouchTarget, on_pair_a, this);
    remote_b_status = make_label(remotes, tokens::font_body(), tokens::text());
    make_button(remotes, "PAIR TEAM B", tokens::kTouchTarget, on_pair_b, this);

    // Bottom bar: diagnostics + start.
    lv_obj_t* bottom = lv_obj_create(root);
    lv_obj_set_size(bottom, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom, 0, 0);
    lv_obj_set_style_pad_all(bottom, 0, 0);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);

    make_button(bottom, LV_SYMBOL_LIST " DIAGNOSTICS", tokens::kTouchTarget, on_diagnostics, this);
    lv_obj_t* start = make_button(bottom, LV_SYMBOL_PLAY " START MATCH", tokens::kOrganizerTarget,
                                  on_start, this);
    lv_obj_set_style_bg_color(start, tokens::success(), 0);
    lv_obj_set_width(start, 320);

    keyboard = lv_keyboard_create(root);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
}

MatchSettings SetupScreen::read_settings() const {
    MatchSettings settings{};
    settings.court_label = lv_textarea_get_text(court_field);
    settings.team_a_name = lv_textarea_get_text(team_a_field);
    settings.team_b_name = lv_textarea_get_text(team_b_field);
    settings.players_a = lv_textarea_get_text(players_a_field);
    settings.players_b = lv_textarea_get_text(players_b_field);
    settings.preset_index = static_cast<int>(lv_dropdown_get_selected(preset_dropdown));
    settings.first_server =
        lv_dropdown_get_selected(server_dropdown) == 0 ? TeamId::A : TeamId::B;
    if (settings.court_label.empty()) {
        settings.court_label = "COURT 1";
    }
    return settings;
}

void SetupScreen::update(const MatchSettings& settings, const LiveViewModel& live) {
    // Seed the editable fields once (organizer edits must not be clobbered
    // by refreshes).
    if (!fields_initialized) {
        lv_textarea_set_text(court_field, settings.court_label.c_str());
        lv_textarea_set_text(team_a_field, settings.team_a_name.c_str());
        lv_textarea_set_text(team_b_field, settings.team_b_name.c_str());
        lv_textarea_set_text(players_a_field, settings.players_a.c_str());
        lv_textarea_set_text(players_b_field, settings.players_b.c_str());
        lv_dropdown_set_selected(preset_dropdown, settings.preset_index);
        lv_dropdown_set_selected(server_dropdown, settings.first_server == TeamId::A ? 0 : 1);
        fields_initialized = true;
    }

    const auto remote_line = [](const TeamPanelModel& team, const char* prefix) {
        if (!team.remote_assigned) {
            return std::string(prefix) + ": no remote paired";
        }
        return std::string(prefix) + (team.remote_ok ? ": remote OK" : ": remote paired");
    };
    set_text(remote_a_status, remote_line(live.team_a, "Team A"));
    set_text(remote_b_status, remote_line(live.team_b, "Team B"));
}

}  // namespace padel::ui::internal
