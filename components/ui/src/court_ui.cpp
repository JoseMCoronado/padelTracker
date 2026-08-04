#include "padel/ui/court_ui.hpp"

#include "padel/ui/tokens.hpp"
#include "screens.hpp"

namespace padel::ui {

void init_theme() {
    lv_theme_default_init(lv_disp_get_default(), tokens::team_a(), tokens::team_b(),
                          /*dark=*/true, tokens::font_body());
}

CourtUi::CourtUi() : screens_(std::make_unique<internal::Screens>()) {}
CourtUi::~CourtUi() = default;

void CourtUi::create(UiCallbacks callbacks) {
    internal::Screens& s = *screens_;
    s.shared.callbacks = std::move(callbacks);
    s.live.create(&s.shared);
    s.setup.create(&s.shared);
    s.complete.create(&s.shared);
    s.pairing.create(&s.shared);
    s.diagnostics.create(&s.shared);
    s.recovery.create(&s.shared);
    s.created = true;
    lv_scr_load(s.setup.root);
    s.current = Screen::Setup;
}

void CourtUi::render(const UiModel& model) {
    internal::Screens& s = *screens_;
    if (!s.created) {
        return;
    }

    if (model.screen != s.current) {
        s.live.close_dialogs();
        s.live.close_organizer_menu();
        lv_obj_t* root = nullptr;
        switch (model.screen) {
            case Screen::Setup: root = s.setup.root; break;
            case Screen::Live: root = s.live.root; break;
            case Screen::MatchComplete: root = s.complete.root; break;
            case Screen::Pairing: root = s.pairing.root; break;
            case Screen::Diagnostics: root = s.diagnostics.root; break;
            case Screen::Recovery: root = s.recovery.root; break;
        }
        lv_scr_load(root);
        s.current = model.screen;
    }

    switch (s.current) {
        case Screen::Setup:
            s.setup.update(model.settings, model.live);
            break;
        case Screen::Live:
            s.live.update(model.live);
            break;
        case Screen::MatchComplete:
            s.complete.update(model.complete);
            break;
        case Screen::Pairing:
            s.pairing.update(model.pairing);
            break;
        case Screen::Diagnostics:
            s.diagnostics.update(model.diagnostics);
            break;
        case Screen::Recovery:
            s.recovery.update(model.recovery);
            break;
    }
}

}  // namespace padel::ui
