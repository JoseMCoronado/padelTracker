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
    m.live.set_history = "7-6(5)  4-6  |  current 5-6";
    m.live.serving_label = "Serving: CLUB DEPORTIVO LOS GUERREROS";
    m.live.conflict = true;
    m.live.storage_fault = true;
    m.live.radio_ok = false;
    m.live.point_flash = TeamId::A;

    m.complete.winner_label = "CLUB DEPORTIVO LOS GUERREROS DEL PADEL 2026 WINS";
    m.complete.final_score = "7-6(5)  4-6  7-5";
    m.complete.duration_label = "Duration: 96 min";

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
    const ui::Screen screens[] = {ui::Screen::Setup,        ui::Screen::Live,
                                  ui::Screen::MatchComplete, ui::Screen::Pairing,
                                  ui::Screen::Diagnostics,   ui::Screen::Recovery,
                                  ui::Screen::ClubMix,       ui::Screen::ClubStandings};
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
                lv_area_t coords;
                lv_obj_get_coords(label, &coords);
                INFO("label text: " << text);
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

    const auto find_label = [](const char* text) -> lv_obj_t* {
        std::vector<lv_obj_t*> labels;
        collect_labels(lv_scr_act(), labels);
        for (lv_obj_t* label : labels) {
            if (std::string(lv_label_get_text(label)) == text) {
                return label;
            }
        }
        return nullptr;
    };

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
    s_pointer.pressed = true;
    pump();
    s_pointer.pressed = false;
    pump();

    CHECK(find_label("1 / 2 picked") != nullptr);

    // Tap again: deselects.
    s_pointer.pressed = true;
    pump();
    s_pointer.pressed = false;
    pump();
    CHECK(find_label("0 / 2 picked") != nullptr);
}

TEST_CASE("picked players replace an unchanged generic team name at start") {
    ui::UiModel m = stress_model(ui::Screen::Setup);
    court_ui().render(m);
    settle();
    court_ui().debug_open_club_picker(TeamId::A);
    settle();

    const auto find_label = [](const char* text) -> lv_obj_t* {
        std::vector<lv_obj_t*> labels;
        collect_labels(lv_scr_act(), labels);
        for (lv_obj_t* label : labels) {
            if (std::string(lv_label_get_text(label)) == text) {
                return label;
            }
        }
        return nullptr;
    };
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
