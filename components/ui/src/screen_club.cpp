// Club round UI: the setup-screen club row + full-screen player picker
// modal, the between-sets mix screen, and the standings screen with the
// automatic coin-flip announcement.
#include <algorithm>
#include <cctype>

#include "padel/ui/tokens.hpp"
#include "screens.hpp"

namespace padel::ui::internal {
namespace {

SetupScreen* setup_self(lv_event_t* e) {
    return static_cast<SetupScreen*>(lv_event_get_user_data(e));
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string uppercased(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

std::string pair_label(const std::vector<ClubPlayer>& picked) {
    if (picked.empty()) {
        return "tap to pick 2 players";
    }
    std::string label = uppercased(picked[0].name);
    if (picked.size() > 1) {
        label += " & " + uppercased(picked[1].name);
    } else {
        label += "  (pick 1 more)";
    }
    return label;
}

lv_obj_t* transparent_row(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, tokens::kSpaceM, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

}  // namespace

// --- Setup screen: club row ---------------------------------------------------

bool SetupScreen::club_mode() const {
    return preset_dropdown != nullptr &&
           static_cast<int>(lv_dropdown_get_selected(preset_dropdown)) == kClubRoundPreset;
}

void SetupScreen::create_club_row(lv_obj_t* parent) {
    club_panel = make_panel(parent);
    lv_obj_set_size(club_panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(club_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(club_panel, tokens::kSpaceS, 0);
    lv_obj_t* panel = club_panel;

    club_title_label = make_label(panel, tokens::font_heading(), tokens::text());
    lv_label_set_text(club_title_label, "PLAYERS  (optional)");

    lv_obj_t* teams = transparent_row(panel);
    club_pick_a_button = make_button(teams, "PICK TEAM A", tokens::kTouchTarget,
                                     [](lv_event_t* e) { setup_self(e)->open_picker(TeamId::A); },
                                     this);
    lv_obj_set_style_bg_color(club_pick_a_button, tokens::team_a(), 0);
    club_a_label = make_label(teams, tokens::font_body(), tokens::text_muted());
    lv_label_set_text(club_a_label, "tap to pick 2 players");

    club_pick_b_button = make_button(teams, "PICK TEAM B", tokens::kTouchTarget,
                                     [](lv_event_t* e) { setup_self(e)->open_picker(TeamId::B); },
                                     this);
    lv_obj_set_style_bg_color(club_pick_b_button, tokens::team_b(), 0);
    club_b_label = make_label(teams, tokens::font_body(), tokens::text_muted());
    lv_label_set_text(club_b_label, "tap to pick 2 players");

    club_hint_label = make_label(panel, tokens::font_body(), tokens::warning());
}

void SetupScreen::update_club(const ClubViewModel& club) {
    // The host re-renders continuously; recreating the tile widgets every
    // frame would destroy a tile mid-press and swallow the tap. Only rebuild
    // when the roster content actually changed.
    const auto same_roster = [&]() {
        if (roster_snapshot.size() != club.roster.size()) {
            return false;
        }
        for (std::size_t i = 0; i < roster_snapshot.size(); ++i) {
            if (roster_snapshot[i].id != club.roster[i].id ||
                roster_snapshot[i].name != club.roster[i].name) {
                return false;
            }
        }
        return true;
    };
    const bool roster_changed = !same_roster();
    if (roster_changed) {
        roster_snapshot = club.roster;
    }

    // NEW ROUND: host bumped suggestion_seq with Top 2 on opposite teams.
    if (club.suggestion_seq != 0 && club.suggestion_seq != applied_suggestion_seq) {
        applied_suggestion_seq = club.suggestion_seq;
        picked_a = club.suggested_a;
        picked_b = club.suggested_b;
        club_hint_local.clear();
    }

    // NEW PLAYER round trip: the host persisted it and republished the
    // roster; select it for the team being picked.
    if (!pending_new_player.empty()) {
        const std::string key = lowered(pending_new_player);
        for (const ClubPlayer& player : roster_snapshot) {
            if (lowered(player.name) == key) {
                pending_new_player.clear();
                club_hint_local.clear();
                toggle_pick(player);
                break;
            }
        }
    }

    set_text(club_a_label, pair_label(picked_a));
    set_text(club_b_label, pair_label(picked_b));
    set_text(club_hint_label, !club_hint_local.empty() ? club_hint_local : club.setup_hint);

    // Picking players is available in every mode; club round is the one
    // mode that requires a full 2v2 (standings need all four).
    const bool in_club_mode = club_mode();
    set_text(start_label, in_club_mode ? LV_SYMBOL_PLAY " START CLUB ROUND"
                                       : LV_SYMBOL_PLAY " START MATCH");
    set_text(club_title_label,
             in_club_mode ? "CLUB ROUND PLAYERS  (2 per team; top 2 / bottom 2 after the mix)"
                          : "PLAYERS  (optional)");

    if (roster_changed && picker_overlay != nullptr &&
        !lv_obj_has_flag(picker_overlay, LV_OBJ_FLAG_HIDDEN)) {
        rebuild_picker_grid();
    }
}

void SetupScreen::on_start_pressed() {
    if (club_mode()) {
        if (picked_a.size() != 2 || picked_b.size() != 2) {
            club_hint_local = "Pick 2 players for each team first";
            set_text(club_hint_label, club_hint_local);
            return;
        }
        club_hint_local.clear();
        if (shared->callbacks.start_club_round) {
            shared->callbacks.start_club_round(
                {picked_a[0], picked_a[1], picked_b[0], picked_b[1]}, read_settings());
        }
        return;
    }
    if (shared->callbacks.start_match) {
        shared->callbacks.start_match(read_settings());
    }
}

// --- Player picker modal --------------------------------------------------------

void SetupScreen::open_picker(TeamId team) {
    picking = team;

    if (picker_overlay == nullptr) {
        picker_overlay = lv_obj_create(root);
        lv_obj_set_size(picker_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_add_flag(picker_overlay, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(picker_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_color(picker_overlay, tokens::bg(), 0);
        lv_obj_set_style_bg_opa(picker_overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(picker_overlay, 0, 0);
        lv_obj_set_style_pad_all(picker_overlay, tokens::kSpaceM, 0);
        lv_obj_set_flex_flow(picker_overlay, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(picker_overlay, tokens::kSpaceS, 0);
        lv_obj_clear_flag(picker_overlay, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* header = transparent_row(picker_overlay);
        picker_title = make_label(header, tokens::font_large(), tokens::text());
        picker_count_label = make_label(header, tokens::font_heading(), tokens::text_muted());

        lv_obj_t* controls = transparent_row(picker_overlay);
        picker_search = lv_textarea_create(controls);
        lv_textarea_set_one_line(picker_search, true);
        lv_textarea_set_placeholder_text(picker_search, "Search or type a new name");
        lv_obj_set_width(picker_search, 360);
        lv_obj_set_style_bg_color(picker_search, tokens::surface_raised(), 0);
        lv_obj_set_style_text_font(picker_search, tokens::font_heading(), 0);
        lv_obj_add_event_cb(
            picker_search,
            [](lv_event_t* e) {
                SetupScreen* s = setup_self(e);
                const lv_event_code_t code = lv_event_get_code(e);
                if (code == LV_EVENT_VALUE_CHANGED) {
                    s->rebuild_picker_grid();
                } else if (code == LV_EVENT_FOCUSED) {
                    lv_keyboard_set_textarea(s->picker_keyboard, s->picker_search);
                    lv_obj_clear_flag(s->picker_keyboard, LV_OBJ_FLAG_HIDDEN);
                } else if (code == LV_EVENT_DEFOCUSED) {
                    lv_obj_add_flag(s->picker_keyboard, LV_OBJ_FLAG_HIDDEN);
                }
            },
            LV_EVENT_ALL, this);

        make_button(controls, LV_SYMBOL_PLUS " NEW PLAYER", tokens::kTouchTarget,
                    [](lv_event_t* e) { setup_self(e)->add_new_player_from_search(); }, this);
        make_button(controls, "ADD GUEST", tokens::kTouchTarget,
                    [](lv_event_t* e) { setup_self(e)->add_guest(); }, this);
        lv_obj_t* done = make_button(controls, LV_SYMBOL_OK " DONE", tokens::kTouchTarget,
                                     [](lv_event_t* e) { setup_self(e)->close_picker(); }, this);
        lv_obj_set_style_bg_color(done, tokens::success(), 0);

        picker_grid = lv_obj_create(picker_overlay);
        lv_obj_set_size(picker_grid, LV_PCT(100), 10);
        lv_obj_set_flex_grow(picker_grid, 1);
        lv_obj_set_style_bg_opa(picker_grid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(picker_grid, 0, 0);
        lv_obj_set_style_pad_all(picker_grid, 0, 0);
        lv_obj_set_style_pad_row(picker_grid, tokens::kSpaceS, 0);
        lv_obj_set_style_pad_column(picker_grid, tokens::kSpaceS, 0);
        lv_obj_set_flex_flow(picker_grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_add_flag(picker_grid, LV_OBJ_FLAG_SCROLLABLE);

        picker_keyboard = lv_keyboard_create(picker_overlay);
        lv_obj_add_flag(picker_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(picker_keyboard, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(picker_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    }

    lv_obj_clear_flag(picker_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(picker_overlay);
    lv_textarea_set_text(picker_search, "");
    club_hint_local.clear();
    rebuild_picker_grid();
}

void SetupScreen::close_picker() {
    if (picker_overlay != nullptr) {
        lv_obj_add_flag(picker_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(picker_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    set_text(club_a_label, pair_label(picked_a));
    set_text(club_b_label, pair_label(picked_b));
}

void SetupScreen::rebuild_picker_grid() {
    if (picker_grid == nullptr) {
        return;
    }
    lv_obj_clean(picker_grid);

    const std::vector<ClubPlayer>& mine = picking == TeamId::A ? picked_a : picked_b;

    set_text(picker_title,
             picking == TeamId::A ? "PICK 2 PLAYERS - TEAM A" : "PICK 2 PLAYERS - TEAM B");

    const std::string needle = lowered(lv_textarea_get_text(picker_search));
    picker_items.clear();
    // Guests already picked stay visible so they can be deselected.
    for (const ClubPlayer& guest : mine) {
        if (guest.guest) {
            picker_items.push_back(guest);
        }
    }
    for (const ClubPlayer& player : roster_snapshot) {
        if (needle.empty() || lowered(player.name).find(needle) != std::string::npos) {
            picker_items.push_back(player);
        }
    }

    for (std::size_t i = 0; i < picker_items.size(); ++i) {
        const ClubPlayer& player = picker_items[i];
        lv_obj_t* tile = make_button(
            picker_grid, uppercased(player.name).c_str(), tokens::kOrganizerTarget,
            [](lv_event_t* e) {
                SetupScreen* s = setup_self(e);
                const auto index =
                    reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(lv_event_get_target(e)));
                if (index < s->picker_items.size()) {
                    s->toggle_pick(s->picker_items[index]);
                    // Restyle in place: rebuilding here would delete the very
                    // widget this event is dispatching on.
                    s->refresh_picker_tiles();
                }
            },
            this);
        lv_obj_set_user_data(tile, reinterpret_cast<void*>(static_cast<std::uintptr_t>(i)));
        lv_obj_set_width(tile, 226);
        // Long names must not escape the fixed-width tile: ellipsize.
        lv_obj_t* tile_label = lv_obj_get_child(tile, 0);
        lv_label_set_long_mode(tile_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(tile_label, LV_PCT(100));
        lv_obj_set_style_text_align(tile_label, LV_TEXT_ALIGN_CENTER, 0);
    }

    if (picker_items.empty()) {
        lv_obj_t* empty = make_label(picker_grid, tokens::font_body(), tokens::text_muted());
        lv_label_set_text(empty, "No players match - use NEW PLAYER to add them");
    }

    refresh_picker_tiles();
}

void SetupScreen::refresh_picker_tiles() {
    if (picker_grid == nullptr) {
        return;
    }
    const std::vector<ClubPlayer>& mine = picking == TeamId::A ? picked_a : picked_b;
    const std::vector<ClubPlayer>& theirs = picking == TeamId::A ? picked_b : picked_a;

    set_text(picker_count_label, std::to_string(mine.size()) + " / 2 picked");

    const auto contains = [](const std::vector<ClubPlayer>& list, std::uint32_t id) {
        return std::any_of(list.begin(), list.end(),
                           [id](const ClubPlayer& p) { return p.id == id; });
    };

    for (std::uint32_t c = 0; c < lv_obj_get_child_cnt(picker_grid); ++c) {
        lv_obj_t* tile = lv_obj_get_child(picker_grid, c);
        if (!lv_obj_check_type(tile, &lv_btn_class)) {
            continue;  // the "no players match" label
        }
        const auto index =
            reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(tile));
        if (index >= picker_items.size()) {
            continue;
        }
        const ClubPlayer& player = picker_items[index];
        if (contains(mine, player.id)) {
            lv_obj_clear_state(tile, LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(
                tile, picking == TeamId::A ? tokens::team_a() : tokens::team_b(), 0);
        } else if (contains(theirs, player.id)) {
            lv_obj_add_state(tile, LV_STATE_DISABLED);  // already on the other team
            lv_obj_set_style_bg_color(tile, tokens::surface(), 0);
        } else {
            lv_obj_clear_state(tile, LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(tile, tokens::surface_raised(), 0);
        }
    }

    // Keep the setup-row pair labels live behind the modal too.
    set_text(club_a_label, pair_label(picked_a));
    set_text(club_b_label, pair_label(picked_b));
}

void SetupScreen::toggle_pick(const ClubPlayer& player) {
    std::vector<ClubPlayer>& mine = picking == TeamId::A ? picked_a : picked_b;
    const std::vector<ClubPlayer>& theirs = picking == TeamId::A ? picked_b : picked_a;

    const auto it = std::find_if(mine.begin(), mine.end(),
                                 [&](const ClubPlayer& p) { return p.id == player.id; });
    if (it != mine.end()) {
        mine.erase(it);
        return;
    }
    const bool on_other_team = std::any_of(theirs.begin(), theirs.end(), [&](const ClubPlayer& p) {
        return p.id == player.id;
    });
    if (on_other_team || mine.size() >= 2) {
        return;
    }
    mine.push_back(player);
}

void SetupScreen::add_new_player_from_search() {
    const std::string name = lv_textarea_get_text(picker_search);
    if (name.empty()) {
        set_text(picker_count_label, "Type the new name in the search box first");
        return;
    }
    pending_new_player = name;  // selected when the host republishes the roster
    if (shared->callbacks.create_player) {
        shared->callbacks.create_player(name);
    }
    lv_textarea_set_text(picker_search, "");
}

void SetupScreen::add_guest() {
    // Session-scoped sentinel ids well above the persisted range.
    ++guest_counter;
    ClubPlayer guest{0xFFFF0000u + static_cast<std::uint32_t>(guest_counter),
                     guest_counter == 1 ? "GUEST" : "GUEST " + std::to_string(guest_counter),
                     true};
    toggle_pick(guest);
    rebuild_picker_grid();
}

// --- Club mix screen -------------------------------------------------------------

void ClubMixScreen::create(Shared* shared_state) {
    shared = shared_state;
    root = make_screen_root();

    lv_obj_t* column = lv_obj_create(root);
    lv_obj_set_size(column, LV_PCT(80), LV_SIZE_CONTENT);
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

    lv_obj_t* title = make_label(column, tokens::font_large(), tokens::text());
    lv_label_set_text(title, "SET 1 DONE - MIX IT UP");

    detail_label = make_label(column, tokens::font_heading(), tokens::text_muted());

    team_a_label = make_label(column, tokens::font_banner(), tokens::team_a());
    lv_label_set_long_mode(team_a_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(team_a_label, LV_PCT(100));
    lv_obj_set_style_text_align(team_a_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* versus = make_label(column, tokens::font_large(), tokens::text_muted());
    lv_label_set_text(versus, "vs");

    team_b_label = make_label(column, tokens::font_banner(), tokens::team_b());
    lv_label_set_long_mode(team_b_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(team_b_label, LV_PCT(100));
    lv_obj_set_style_text_align(team_b_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* start = make_button(
        column, LV_SYMBOL_PLAY " START SET 2", tokens::kOrganizerTarget,
        [](lv_event_t* e) {
            auto* s = static_cast<ClubMixScreen*>(lv_event_get_user_data(e));
            if (s->shared->callbacks.club_next_set) s->shared->callbacks.club_next_set();
        },
        this);
    lv_obj_set_style_bg_color(start, tokens::success(), 0);
}

void ClubMixScreen::update(const ClubViewModel& model) {
    set_text(detail_label, model.mix_detail);
    set_text(team_a_label, model.mix_team_a);
    set_text(team_b_label, model.mix_team_b);
}

// --- Club standings screen --------------------------------------------------------

void ClubStandingsScreen::create(Shared* shared_state) {
    shared = shared_state;
    root = make_screen_root();
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, tokens::kSpaceS, 0);

    lv_obj_t* title = make_label(root, tokens::font_large(), tokens::text());
    lv_label_set_text(title, "CLUB ROUND RESULT");

    coin_label = make_label(root, tokens::font_heading(), tokens::warning());

    for (Row& row : rows) {
        row.panel = make_panel(root);
        lv_obj_set_size(row.panel, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row.panel, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row.panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row.panel, tokens::kSpaceL, 0);

        row.rank = make_label(row.panel, tokens::font_large(), tokens::text_muted());
        row.name = make_label(row.panel, tokens::font_large(), tokens::text());
        lv_obj_set_flex_grow(row.name, 1);
        row.record = make_label(row.panel, tokens::font_heading(), tokens::text_muted());
        row.tag = make_label(row.panel, tokens::font_heading(), tokens::success());
    }

    lv_obj_t* buttons = transparent_row(root);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t* again = make_button(
        buttons, LV_SYMBOL_REFRESH " NEW ROUND", tokens::kOrganizerTarget,
        [](lv_event_t* e) {
            auto* s = static_cast<ClubStandingsScreen*>(lv_event_get_user_data(e));
            if (s->shared->callbacks.club_new_round) s->shared->callbacks.club_new_round();
        },
        this);
    lv_obj_set_style_bg_color(again, tokens::success(), 0);
    make_button(
        buttons, "DONE", tokens::kOrganizerTarget,
        [](lv_event_t* e) {
            auto* s = static_cast<ClubStandingsScreen*>(lv_event_get_user_data(e));
            if (s->shared->callbacks.club_done) s->shared->callbacks.club_done();
        },
        this);
}

void ClubStandingsScreen::update(const ClubViewModel& model) {
    set_text(coin_label, model.coin_announcement);
    if (model.coin_announcement.empty()) {
        lv_obj_add_flag(coin_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(coin_label, LV_OBJ_FLAG_HIDDEN);
    }

    for (std::size_t i = 0; i < 4; ++i) {
        Row& row = rows[i];
        if (i >= model.standings.size()) {
            lv_obj_add_flag(row.panel, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(row.panel, LV_OBJ_FLAG_HIDDEN);
        const ClubStandingRowModel& data = model.standings[i];
        set_text(row.rank, data.rank);
        set_text(row.name, data.name + (data.coin ? "  (coin flip)" : ""));
        set_text(row.record, data.record);
        set_text(row.tag, data.top2 ? "TOP 2" : "BOTTOM 2");
        lv_obj_set_style_text_color(row.tag, data.top2 ? tokens::success() : tokens::text_muted(),
                                    0);
        lv_obj_set_style_bg_color(row.panel,
                                  data.top2 ? tokens::surface_raised() : tokens::surface(), 0);
    }
}

}  // namespace padel::ui::internal
