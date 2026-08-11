// Headless LVGL render checks (spec 18.6 subset): every screen builds and
// paints at 1024x600 with stress content (long names, special states), and
// no visible label escapes the screen bounds.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "lvgl.h"
#include "padel/ui/court_ui.hpp"
#include "padel/ui/tokens.hpp"

using namespace padel;

namespace {

constexpr int kWidth = 1024;
constexpr int kHeight = 600;

// Scripted pointer "touch" for interaction tests.
struct {
    lv_point_t point{0, 0};
    bool pressed = false;
} s_pointer;

std::vector<lv_color_t>& framebuffer() {
    static std::vector<lv_color_t> buf(static_cast<std::size_t>(kWidth) * kHeight);
    return buf;
}

void ensure_lvgl() {
    static bool done = [] {
        lv_init();
        static lv_disp_draw_buf_t draw_buf;
        lv_disp_draw_buf_init(&draw_buf, framebuffer().data(), nullptr, framebuffer().size());
        static lv_disp_drv_t driver;
        lv_disp_drv_init(&driver);
        driver.hor_res = kWidth;
        driver.ver_res = kHeight;
        driver.draw_buf = &draw_buf;
        driver.flush_cb = [](lv_disp_drv_t* d, const lv_area_t*, lv_color_t*) {
            lv_disp_flush_ready(d);
        };
        lv_disp_drv_register(&driver);

        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = [](lv_indev_drv_t*, lv_indev_data_t* data) {
            data->point = s_pointer.point;
            data->state = s_pointer.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        };
        lv_indev_drv_register(&indev_drv);

        ui::init_theme();
        return true;
    }();
    (void)done;
}

ui::CourtUi& court_ui() {
    static ui::CourtUi* instance = [] {
        ensure_lvgl();
        auto* ui_ptr = new ui::CourtUi();
        ui_ptr->create(ui::UiCallbacks{});  // no-op callbacks: render-only checks
        return ui_ptr;
    }();
    return *instance;
}

void settle() {
    for (int i = 0; i < 20; ++i) {
        lv_tick_inc(16);
        lv_timer_handler();
    }
}

void collect_labels(lv_obj_t* obj, std::vector<lv_obj_t*>& out) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        out.push_back(obj);
    }
    for (std::uint32_t i = 0; i < lv_obj_get_child_cnt(obj); ++i) {
        collect_labels(lv_obj_get_child(obj, i), out);
    }
}

// Every visible label must stay inside the screen (no horizontal clipping
// off-screen; vertical scroll containers excluded by construction).
void require_labels_in_bounds() {
    std::vector<lv_obj_t*> labels;
    collect_labels(lv_scr_act(), labels);
    REQUIRE(!labels.empty());
    for (lv_obj_t* label : labels) {
        lv_area_t coords;
        lv_obj_get_coords(label, &coords);
        INFO("label text: " << lv_label_get_text(label));
        CHECK(coords.x1 >= 0);
        CHECK(coords.x2 < kWidth);
    }
}

lv_obj_t* find_label(const char* text) {
    std::vector<lv_obj_t*> labels;
    collect_labels(lv_scr_act(), labels);
    for (lv_obj_t* label : labels) {
        if (std::string(lv_label_get_text(label)) == text) {
            return label;
        }
    }
    return nullptr;
}

// The screen actually painted something beyond the background color.
void require_screen_painted() {
    std::size_t non_bg = 0;
    const lv_color_t bg = ui::tokens::bg();
    for (const lv_color_t& px : framebuffer()) {
        if (px.full != bg.full) {
            ++non_bg;
        }
    }
    CHECK(non_bg > 5000);
}

ui::UiModel stress_model(ui::Screen screen) {
    ui::UiModel m{};
    m.screen = screen;

    ui::TeamPanelModel a{};
    a.name = "CLUB DEPORTIVO LOS GUERREROS DEL PADEL 2026";  // deliberately absurd
    a.players = "MAXIMILIANO ALEJANDRO / SEBASTIAN RODRIGUEZ";
    a.points = "40";
    a.games = "6";
    a.sets = "1";
    a.serving = true;
    a.remote_assigned = true;
    a.remote_ok = true;
    ui::TeamPanelModel b = a;
    b.points = "AD";
    b.serving = false;
    b.remote_ok = false;

    m.live.court_label = "COURT 12 - CENTER COURT PREMIUM";
    m.live.mode_label = "STANDARD / ADV / MTB";
    m.live.status_label = "LIVE";
    m.live.special_label = "GOLDEN POINT";
    m.live.team_a = a;
    m.live.team_b = b;
    m.live.scoreboard.name_a = "MAXIMILIANO ALEJANDRO / SEBASTIAN RODRIGUEZ";
    m.live.scoreboard.name_b = "CLUB DEPORTIVO LOS GUERREROS DEL PADEL 2026";
    m.live.scoreboard.serving = TeamId::A;
    // Five sets is the longest board the domain can produce.
    m.live.scoreboard.columns = {{"7", "6", "", "(5)", false, TeamId::A},
                                 {"4", "6", "", "", false, TeamId::B},
                                 {"6", "7", "(9)", "", false, TeamId::B},
                                 {"6", "4", "", "", false, TeamId::A},
                                 {"5", "6", "", "", true, std::nullopt}};
    m.live.conflict = true;
    m.live.storage_fault = true;
    m.live.radio_ok = false;
    m.live.point_flash = TeamId::A;

    m.complete.winner_label = "CLUB DEPORTIVO LOS GUERREROS DEL PADEL 2026 WINS";
    m.complete.final_score = "7-6(5)  4-6  7-5";
    m.complete.duration_label = "Duration: 96 min";

    m.summary.title = "CLUB SET 1 COMPLETE";
    m.summary.winner_label = "CLUB DEPORTIVO LOS GUERREROS DEL PADEL 2026 WIN";
    m.summary.scoreboard = m.live.scoreboard;
    m.summary.scoreboard.serving.reset();
    m.summary.stats = {{"Duration", "96 min"},
                       {"Points played", "184"},
                       {"MAXIMILIANO ALEJANDRO / SEBASTIAN RODRIGUEZ", "97  (53%)"},
                       {"CLUB DEPORTIVO LOS GUERREROS DEL PADEL 2026", "87  (47%)"},
                       {"Best run - MAXIMILIANO ALEJANDRO / SEBASTIAN RODRIGUEZ", "7 in a row"}};
    m.summary.continue_label = "MIX IT UP";

    m.pairing.team_label = "Pairing: TEAM A";
    m.pairing.instruction = "Hold the remote button for 5 seconds to enter pairing mode.";
    m.pairing.candidate_label = "Remote 3F92 requests pairing";
    m.pairing.awaiting_confirm = true;
    m.pairing.seconds_left = 22;

    m.diagnostics.rows = {{"Firmware", "native tests"}, {"Accepted", "1042"},
                          {"Duplicates", "97"},         {"Rejected", "3"},
                          {"Journal size", "18324 B"},  {"State revision", "214"}};
    m.diagnostics.recent_log_lines = {"match.point_accepted team=A rev=214",
                                      "storage.commit_failed err=append"};

    m.recovery.message = "A match was in progress when power was lost.";
    m.recovery.detail = "Journal: 214 events recovered, tail damaged (truncated).";
    m.recovery.corrupt_tail = true;

    m.club.roster = {{1, "Jose", false},
                     {2, "Zoe", false},
                     {3, "William", false},
                     {4, "Szewei", false},
                     {5, "Lewis", false},
                     {6, "Luigi", false},
                     {7, "Maximiliano Alejandro Rodriguez", false}};
    m.club.setup_hint = "ADRIEN & LEWIS were Top 2 last round - split them up";
    m.club.mix_detail = "MAXIMILIANO ALEJANDRO & LEWIS took set 1 (3-2)";
    m.club.mix_team_a = "MAXIMILIANO ALEJANDRO & LOUIS";
    m.club.mix_team_b = "LEWIS & LUIGI";
    m.club.standings = {{"1", "Maximiliano Alejandro Rodriguez", "2 WINS  +4", true, false},
                        {"2", "Lewis", "1 WIN  +0", true, true},
                        {"3", "Louis", "1 WIN  +0", false, false},
                        {"4", "Luigi", "0 WINS  -4", false, false}};
    m.club.coin_announcement = "COIN FLIP: LEWIS takes the last TOP 2 spot";
    return m;
}

}  // namespace

TEST_CASE("every screen renders at 1024x600 with stress content, labels in bounds") {
    const ui::Screen screens[] = {ui::Screen::Setup,         ui::Screen::Live,
                                  ui::Screen::MatchSummary,  ui::Screen::MatchComplete,
                                  ui::Screen::Pairing,       ui::Screen::Diagnostics,
                                  ui::Screen::Recovery,      ui::Screen::ClubMix,
                                  ui::Screen::ClubStandings};
    for (const ui::Screen screen : screens) {
        INFO("screen index: " << static_cast<int>(screen));
        court_ui().render(stress_model(screen));
        settle();
        require_labels_in_bounds();
        require_screen_painted();
    }
}

TEST_CASE("setup screen fits vertically in both modes: bottom bar on screen") {
    const auto check_bottom_bar = [](const char* start_text) {
        std::vector<lv_obj_t*> labels;
        collect_labels(lv_scr_act(), labels);
        int checked = 0;
        for (lv_obj_t* label : labels) {
            const std::string text = lv_label_get_text(label);
            // The bottom action bar is the lowest content; if it fits, the
            // whole non-scrollable setup screen fits.
            if (text.find(start_text) != std::string::npos ||
                text.find("DIAGNOSTICS") != std::string::npos) {
                // The button, not just its text: a tap target whose lower half
                // is off the panel is still a broken layout.
                lv_area_t coords;
                lv_obj_get_coords(lv_obj_get_parent(label), &coords);
                INFO("button text: " << text);
                CHECK(coords.y1 >= 0);
                CHECK(coords.y2 < kHeight);
                ++checked;
            }
        }
        CHECK(checked == 2);
    };

    court_ui().render(stress_model(ui::Screen::Setup));
    settle();
    check_bottom_bar("START MATCH");

    // Club round mode adds the player-pick row; it must fit too.
    court_ui().debug_select_preset(ui::kClubRoundPreset);
    court_ui().render(stress_model(ui::Screen::Setup));
    settle();
    check_bottom_bar("START CLUB ROUND");

    court_ui().debug_select_preset(0);  // leave standard mode for later tests
}

TEST_CASE("club player picker modal builds, filters render, tiles in bounds") {
    ui::UiModel m = stress_model(ui::Screen::Setup);
    court_ui().render(m);
    settle();

    court_ui().debug_open_club_picker(TeamId::A);
    court_ui().render(m);  // refresh with the roster while the modal is open
    settle();
    require_labels_in_bounds();
    require_screen_painted();

    court_ui().debug_open_club_picker(TeamId::B);
    settle();
    require_labels_in_bounds();
}

TEST_CASE("tapping a roster tile selects the player despite continuous re-renders") {
    ui::UiModel m = stress_model(ui::Screen::Setup);
    court_ui().render(m);
    settle();
    court_ui().debug_open_club_picker(TeamId::A);
    settle();

    lv_obj_t* lewis_label = find_label("LEWIS");
    REQUIRE(lewis_label != nullptr);
    lv_area_t coords;
    lv_obj_get_coords(lv_obj_get_parent(lewis_label), &coords);
    s_pointer.point.x = (coords.x1 + coords.x2) / 2;
    s_pointer.point.y = (coords.y1 + coords.y2) / 2;

    // Press-hold-release while the host keeps re-rendering the same model
    // every frame (this is what the sim and firmware do; a grid rebuild
    // during the press deletes the tile and swallows the tap).
    const auto pump = [&] {
        for (int i = 0; i < 6; ++i) {
            lv_tick_inc(16);
            lv_timer_handler();
            court_ui().render(m);
        }
    };
    const auto tap = [&] {
        s_pointer.pressed = true;
        pump();
        s_pointer.pressed = false;
        pump();
    };
    // Long enough that the next tap is a separate pick, not a double tap.
    const auto idle = [&] {
        for (int i = 0; i < 10; ++i) {
            lv_tick_inc(100);
            lv_timer_handler();
            court_ui().render(m);
        }
    };

    tap();
    CHECK(find_label("1 / 2 picked") != nullptr);

    // A second tap right away is a double tap: the player keeps their place
    // and picks up crown 1 instead of being deselected.
    tap();
    CHECK(find_label("1 / 2 picked") != nullptr);
    CHECK(find_label("LEWIS [1]  (pick 1 more)") != nullptr);

    // Slow tap: deselects.
    idle();
    tap();
    CHECK(find_label("0 / 2 picked") != nullptr);
}

TEST_CASE("picked players replace an unchanged generic team name at start") {
    ui::UiModel m = stress_model(ui::Screen::Setup);
    court_ui().render(m);
    settle();
    court_ui().debug_open_club_picker(TeamId::A);
    settle();

    const auto tap = [&](const char* text) {
        lv_obj_t* label = find_label(text);
        REQUIRE(label != nullptr);
        lv_area_t coords;
        lv_obj_get_coords(lv_obj_get_parent(label), &coords);
        s_pointer.point.x = (coords.x1 + coords.x2) / 2;
        s_pointer.point.y = (coords.y1 + coords.y2) / 2;
        const auto pump = [&] {
            for (int i = 0; i < 6; ++i) {
                lv_tick_inc(16);
                lv_timer_handler();
                court_ui().render(m);
            }
        };
        s_pointer.pressed = true;
        pump();
        s_pointer.pressed = false;
        pump();
    };

    tap("LEWIS");
    tap("LUIGI");
    tap(LV_SYMBOL_OK " DONE");

    // Team A's field was left at the generic default, so the picked names
    // become the team header (club-style format); Team B had no picks and
    // keeps its default.
    const ui::MatchSettings settings = court_ui().debug_read_settings();
    CHECK(settings.team_a_name == "LEWIS & LUIGI");
    CHECK(settings.players_a.empty());
    CHECK(settings.team_b_name == "TEAM B");
    CHECK(settings.players_b.empty());

    // Deselect so later tests start from a clean picker state; with no
    // picks the generic name stays.
    court_ui().debug_open_club_picker(TeamId::A);
    settle();
    tap("LEWIS");
    tap("LUIGI");
    tap(LV_SYMBOL_OK " DONE");
    CHECK(court_ui().debug_read_settings().team_a_name == "TEAM A");
}

TEST_CASE("organizer menu floats over the live screen without reflowing it") {
    ui::UiModel m = stress_model(ui::Screen::Live);
    court_ui().render(m);
    settle();

    lv_obj_t* score = find_label("AD");
    REQUIRE(score != nullptr);
    lv_area_t before;
    lv_obj_get_coords(score, &before);

    court_ui().debug_open_organizer_menu(true);
    court_ui().render(m);
    settle();

    // The menu is a floating modal: the score behind it must not move or
    // shrink to make room for it.
    lv_area_t after;
    lv_obj_get_coords(score, &after);
    CHECK(after.x1 == before.x1);
    CHECK(after.y1 == before.y1);
    CHECK(after.y2 == before.y2);

    require_labels_in_bounds();
    for (const char* row : {"ORGANIZER", "PAUSE", LV_SYMBOL_TRASH " RESET MATCH", "CLOSE"}) {
        lv_obj_t* label = find_label(row);
        INFO("menu row: " << row);
        REQUIRE(label != nullptr);
        lv_area_t coords;
        lv_obj_get_coords(label, &coords);
        CHECK(coords.y1 >= 0);
        CHECK(coords.y2 < kHeight);
    }

    court_ui().debug_open_organizer_menu(false);
    settle();
    CHECK(find_label("CLOSE") == nullptr);
}

TEST_CASE("confirmation dialogs are finger-sized and cancel closes them") {
    ui::UiModel m = stress_model(ui::Screen::Live);
    m.live.undo_preview = TeamId::A;
    m.live.conflict = false;  // its banner has a CANCEL of its own
    court_ui().render(m);
    settle();

    const auto tap = [&](const char* text) {
        lv_obj_t* label = find_label(text);
        REQUIRE(label != nullptr);
        lv_area_t coords;
        lv_obj_get_coords(lv_obj_get_parent(label), &coords);
        s_pointer.point.x = (coords.x1 + coords.x2) / 2;
        s_pointer.point.y = (coords.y1 + coords.y2) / 2;
        const auto pump = [&] {
            for (int i = 0; i < 6; ++i) {
                lv_tick_inc(16);
                lv_timer_handler();
                court_ui().render(m);
            }
        };
        s_pointer.pressed = true;
        pump();
        s_pointer.pressed = false;
        pump();
    };

    // A dialog button is the target an organizer hits mid-match with a wet
    // finger, so it has to stay well above the plain touch minimum.
    const auto require_finger_sized = [](const char* text) {
        lv_obj_t* label = find_label(text);
        INFO("dialog button: " << text);
        REQUIRE(label != nullptr);
        lv_obj_t* button = lv_obj_get_parent(label);
        CHECK(lv_obj_get_height(button) >= ui::tokens::kDialogTarget);
        CHECK(lv_obj_get_width(button) >= 2 * ui::tokens::kDialogTarget);
    };

    court_ui().debug_open_undo_dialog();
    settle();
    require_labels_in_bounds();
    require_finger_sized("CANCEL");
    require_finger_sized("UNDO");
    tap("CANCEL");
    CHECK(find_label("UNDO LAST POINT") == nullptr);

    // Reset keeps its two steps: the first confirm only opens the second.
    court_ui().debug_open_reset_dialog(1);
    settle();
    require_finger_sized("CONTINUE");
    tap("CONTINUE");
    settle();
    REQUIRE(find_label("CONFIRM RESET") != nullptr);
    require_finger_sized("RESET");
    tap("CANCEL");
    CHECK(find_label("CONFIRM RESET") == nullptr);

    // Nothing to undo: the dialog says so and offers only the way out.
    m.live.undo_preview.reset();
    court_ui().render(m);
    settle();
    court_ui().debug_open_undo_dialog();
    settle();
    CHECK(find_label("UNDO") == nullptr);
    require_finger_sized("CLOSE");
    tap("CLOSE");
    CHECK(find_label("UNDO LAST POINT") == nullptr);
}

TEST_CASE("UNPAIR appears only for a team that has a remote, and confirms first") {
    ui::UiModel m = stress_model(ui::Screen::Setup);
    m.live.team_a.remote_assigned = false;
    m.live.team_b.remote_assigned = true;
    court_ui().render(m);
    settle();
    // Hidden buttons are skipped by collect_labels, so absence is invisibility.
    CHECK(find_label("UNPAIR A") == nullptr);
    CHECK(find_label("UNPAIR B") != nullptr);
    CHECK(find_label("NO REMOTE") != nullptr);

    m.live.team_a.remote_assigned = true;
    court_ui().render(m);
    settle();
    CHECK(find_label("UNPAIR A") != nullptr);
    require_labels_in_bounds();

    // Unpairing is a confirmed action, never a single stray tap.
    court_ui().debug_open_unpair_dialog(TeamId::A);
    settle();
    REQUIRE(find_label("UNPAIR TEAM A REMOTE?") != nullptr);
    require_labels_in_bounds();

    lv_obj_t* cancel = find_label("CANCEL");
    REQUIRE(cancel != nullptr);
    lv_area_t coords;
    lv_obj_get_coords(lv_obj_get_parent(cancel), &coords);
    s_pointer.point.x = (coords.x1 + coords.x2) / 2;
    s_pointer.point.y = (coords.y1 + coords.y2) / 2;
    for (const bool pressed : {true, false}) {
        s_pointer.pressed = pressed;
        for (int i = 0; i < 6; ++i) {
            lv_tick_inc(16);
            lv_timer_handler();
            court_ui().render(m);
        }
    }
    CHECK(find_label("UNPAIR TEAM A REMOTE?") == nullptr);
}

TEST_CASE("special scoring states render distinct label text") {
    ui::UiModel m = stress_model(ui::Screen::Live);
    for (const char* special : {"DEUCE", "GOLDEN POINT", "TIEBREAK", "MATCH TIEBREAK"}) {
        m.live.special_label = special;
        court_ui().render(m);
        settle();
        require_labels_in_bounds();
    }
    // Tiebreak digit scores also fit.
    m.live.team_a.points = "18";
    m.live.team_b.points = "17";
    court_ui().render(m);
    settle();
    require_labels_in_bounds();
}
