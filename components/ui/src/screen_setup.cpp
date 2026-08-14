// Setup screen (spec 14.4): court label, team/player names, scoring preset,
// first server, remote pairing status, start.
#include <cctype>

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
    // Routes to start_match or start_club_round (screen_club.cpp).
    self(e)->on_start_pressed();
}

void on_pair_a(lv_event_t* e) {
    SetupScreen* s = self(e);
    if (s->shared->callbacks.begin_pairing) s->shared->callbacks.begin_pairing(TeamId::A);
}

void on_pair_b(lv_event_t* e) {
    SetupScreen* s = self(e);
    if (s->shared->callbacks.begin_pairing) s->shared->callbacks.begin_pairing(TeamId::B);
}

void on_unpair_a(lv_event_t* e) { self(e)->open_unpair_dialog(TeamId::A); }
void on_unpair_b(lv_event_t* e) { self(e)->open_unpair_dialog(TeamId::B); }

void on_dialog_dismiss(lv_event_t* e) { self(e)->close_dialogs(); }

void on_unpair_confirm(lv_event_t* e) {
    SetupScreen* s = self(e);
    const TeamId team = s->unpair_target;
    s->close_dialogs();
    if (s->shared->callbacks.unpair_remote) s->shared->callbacks.unpair_remote(team);
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

    // The battery sits opposite the heading rather than on a row of its own:
    // this screen already has to fit its bottom bar on 600 px, so the readout
    // has to cost no height. Same place the live header keeps it.
    lv_obj_t* title_row = lv_obj_create(root);
    lv_obj_set_size(title_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = make_label(title_row, tokens::font_large(), tokens::text());
    lv_label_set_text(title, "MATCH SETUP");
    battery = make_battery_readout(title_row);

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
    lv_label_set_text(preset_title, "MODE");
    preset_dropdown = lv_dropdown_create(rules_column);
    lv_obj_set_width(preset_dropdown, LV_PCT(100));
    lv_dropdown_set_options(preset_dropdown, dropdown_options(preset_names()).c_str());

    lv_obj_t* server_title = make_label(rules_column, tokens::font_small(), tokens::text_muted());
    lv_label_set_text(server_title, "FIRST SERVER");
    server_dropdown = lv_dropdown_create(rules_column);
    lv_obj_set_width(server_dropdown, LV_PCT(100));
    lv_dropdown_set_options(server_dropdown, "Team A\nTeam B");

    // Player names come from the club picker; team names cover the rest.
    team_a_field = make_field(form, "TEAM A NAME", this);
    team_b_field = make_field(form, "TEAM B NAME", this);

    // Club round toggle + per-team player pickers (screen_club.cpp).
    create_club_row(root);

    // Remote status + pairing entry points.
    lv_obj_t* remotes = make_panel(root);
    lv_obj_set_size(remotes, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(remotes, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(remotes, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // One group per team. Grouping matters here: with all six widgets in a
    // single full-width row, the status labels and the buttons collide once
    // UNPAIR is showing.
    const auto remote_group = [&](const char* pair_text, const char* unpair_text,
                                  lv_event_cb_t on_pair, lv_event_cb_t on_unpair,
                                  lv_obj_t** status, lv_obj_t** unpair) {
        lv_obj_t* group = lv_obj_create(remotes);
        lv_obj_set_size(group, LV_PCT(48), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(group, 0, 0);
        lv_obj_set_style_pad_all(group, 0, 0);
        lv_obj_set_style_pad_column(group, tokens::kSpaceS, 0);
        lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(group, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);

        *status = make_label(group, tokens::font_body(), tokens::text());
        lv_obj_set_flex_grow(*status, 1);

        make_button(group, pair_text, tokens::kTouchTarget, on_pair, this);
        // Hidden until the team actually has a remote; a hidden child
        // collapses out of the row, leaving the PAIR button where it was.
        *unpair = make_button(group, unpair_text, tokens::kTouchTarget, on_unpair, this);
        lv_obj_set_style_bg_color(*unpair, tokens::error(), 0);
        lv_obj_add_flag(*unpair, LV_OBJ_FLAG_HIDDEN);
    };

    remote_group("PAIR TEAM A", "UNPAIR A", on_pair_a, on_unpair_a, &remote_a_status,
                 &unpair_a_button);
    remote_group("PAIR TEAM B", "UNPAIR B", on_pair_b, on_unpair_b, &remote_b_status,
                 &unpair_b_button);

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
    start_label = lv_obj_get_child(start, 0);

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
    // Players come from the roster picker in every mode (empty when none
    // were picked); they show under the team names on the live screen.
    const auto players_of = [](const std::vector<ClubPlayer>& picked) {
        std::string names;
        for (const ClubPlayer& player : picked) {
            if (!names.empty()) {
                names += " / ";
            }
            names += player.name;
        }
        return names;
    };
    settings.players_a = players_of(picked_a);
    settings.players_b = players_of(picked_b);
    // A team name left as the generic default gets replaced by the picked
    // players, so the scoring page header shows real names instead of
    // "TEAM A" with the players in small print underneath. The header uses
    // the club-style format: "JOSE & ZOE".
    const auto to_upper = [](std::string text) {
        for (char& c : text) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return text;
    };
    const auto header_of = [&](const std::vector<ClubPlayer>& picked) {
        std::string names;
        for (const ClubPlayer& player : picked) {
            if (!names.empty()) {
                names += " & ";
            }
            names += to_upper(player.name);
        }
        return names;
    };
    const auto promote_players = [&](std::string& team_name, std::string& players,
                                     const std::vector<ClubPlayer>& picked,
                                     const char* generic) {
        if (picked.empty()) {
            return;
        }
        if (team_name.empty() || to_upper(team_name) == generic) {
            team_name = header_of(picked);
            players.clear();
        }
    };
    promote_players(settings.team_a_name, settings.players_a, picked_a, "TEAM A");
    promote_players(settings.team_b_name, settings.players_b, picked_b, "TEAM B");
    settings.preset_index = static_cast<int>(lv_dropdown_get_selected(preset_dropdown));
    settings.first_server =
        lv_dropdown_get_selected(server_dropdown) == 0 ? TeamId::A : TeamId::B;
    if (settings.court_label.empty()) {
        settings.court_label = "COURT 1";
    }
    return settings;
}

void SetupScreen::update(const MatchSettings& settings, const LiveViewModel& live,
                         const ClubViewModel& club) {
    // Seed the editable fields once (organizer edits must not be clobbered
    // by refreshes).
    if (!fields_initialized) {
        lv_textarea_set_text(court_field, settings.court_label.c_str());
        lv_textarea_set_text(team_a_field, settings.team_a_name.c_str());
        lv_textarea_set_text(team_b_field, settings.team_b_name.c_str());
        lv_dropdown_set_selected(preset_dropdown, settings.preset_index);
        lv_dropdown_set_selected(server_dropdown, settings.first_server == TeamId::A ? 0 : 1);
        fields_initialized = true;
    }

    // Same words and colors as the live screen's remote tags: the buttons
    // beside each status already name the team, and a longer sentence wraps
    // against the pair/unpair buttons sharing the row.
    const auto remote_status = [](lv_obj_t* label, lv_obj_t* unpair,
                                  const TeamPanelModel& team) {
        if (!team.remote_assigned) {
            set_text(label, "NO REMOTE");
            lv_obj_set_style_text_color(label, tokens::text_muted(), 0);
            lv_obj_add_flag(unpair, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        set_text(label, team.remote_ok ? "REMOTE OK" : "REMOTE ?");
        lv_obj_set_style_text_color(
            label, team.remote_ok ? tokens::success() : tokens::warning(), 0);
        lv_obj_clear_flag(unpair, LV_OBJ_FLAG_HIDDEN);
    };
    remote_status(remote_a_status, unpair_a_button, live.team_a);
    remote_status(remote_b_status, unpair_b_button, live.team_b);

    update_battery_readout(battery, live);

    update_club(club);
}

void SetupScreen::open_unpair_dialog(TeamId team) {
    close_dialogs();
    unpair_target = team;
    Dialog dialog = make_dialog(
        root, team == TeamId::A ? "UNPAIR TEAM A REMOTE?" : "UNPAIR TEAM B REMOTE?",
        "The court forgets this remote. Hold its button for 5 seconds to pair it again.",
        on_dialog_dismiss, this);
    add_dialog_button(dialog, "CANCEL", tokens::surface_raised(), on_dialog_dismiss, this);
    add_dialog_button(dialog, "UNPAIR", tokens::error(), on_unpair_confirm, this);
    unpair_dialog = dialog.overlay;
}

void SetupScreen::close_dialogs() {
    // Async delete: closing runs from a button inside the dialog being deleted.
    if (unpair_dialog != nullptr) {
        lv_obj_del_async(unpair_dialog);
        unpair_dialog = nullptr;
    }
}

}  // namespace padel::ui::internal
