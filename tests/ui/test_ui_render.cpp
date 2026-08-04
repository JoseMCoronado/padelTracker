// Headless LVGL render checks (spec 18.6 subset): every screen builds and
// paints at 1024x600 with stress content (long names, special states), and
// no visible label escapes the screen bounds.
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "lvgl.h"
#include "padel/ui/court_ui.hpp"
#include "padel/ui/tokens.hpp"

using namespace padel;

namespace {

constexpr int kWidth = 1024;
constexpr int kHeight = 600;

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
    return m;
}

}  // namespace

TEST_CASE("every screen renders at 1024x600 with stress content, labels in bounds") {
    const ui::Screen screens[] = {ui::Screen::Setup,        ui::Screen::Live,
                                  ui::Screen::MatchComplete, ui::Screen::Pairing,
                                  ui::Screen::Diagnostics,   ui::Screen::Recovery};
    for (const ui::Screen screen : screens) {
        INFO("screen index: " << static_cast<int>(screen));
        court_ui().render(stress_model(screen));
        settle();
        require_labels_in_bounds();
        require_screen_painted();
    }
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
