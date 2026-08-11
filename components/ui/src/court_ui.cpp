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
    s.summary.create(&s.shared);
    s.complete.create(&s.shared);
    s.pairing.create(&s.shared);
    s.diagnostics.create(&s.shared);
    s.recovery.create(&s.shared);
    s.club_mix.create(&s.shared);
    s.club_standings.create(&s.shared);
    s.created = true;
    lv_scr_load(s.setup.root);
    s.current = Screen::Setup;
}

void CourtUi::debug_select_preset(int preset_index) {
    if (screens_->created) {
        lv_dropdown_set_selected(screens_->setup.preset_dropdown,
                                 static_cast<uint16_t>(preset_index));
    }
}

void CourtUi::debug_open_club_picker(TeamId team) {
    if (screens_->created) {
        screens_->setup.open_picker(team);
    }
}

void CourtUi::debug_open_organizer_menu(bool open) {
    if (!screens_->created) return;
    if (open) {
        screens_->live.open_organizer_menu();
    } else {
        screens_->live.close_organizer_menu();
    }
}

void CourtUi::debug_open_undo_dialog() {
    if (screens_->created) {
        screens_->live.open_undo_dialog();
    }
}

void CourtUi::debug_open_reset_dialog(int step) {
    if (screens_->created) {
        screens_->live.open_reset_dialog(step);
    }
}

void CourtUi::debug_open_unpair_dialog(TeamId team) {
    if (screens_->created) {
        screens_->setup.open_unpair_dialog(team);
    }
}

MatchSettings CourtUi::debug_read_settings() const {
    return screens_->created ? screens_->setup.read_settings() : MatchSettings{};
}

void CourtUi::render(const UiModel& model) {
    internal::Screens& s = *screens_;
    if (!s.created) {
        return;
    }

    if (model.screen != s.current) {
        s.live.close_dialogs();
        s.live.close_organizer_menu();
        s.setup.close_dialogs();
        lv_obj_t* root = nullptr;
        switch (model.screen) {
            case Screen::Setup: root = s.setup.root; break;
            case Screen::Live: root = s.live.root; break;
            case Screen::MatchSummary: root = s.summary.root; break;
            case Screen::MatchComplete: root = s.complete.root; break;
            case Screen::Pairing: root = s.pairing.root; break;
            case Screen::Diagnostics: root = s.diagnostics.root; break;
            case Screen::Recovery: root = s.recovery.root; break;
            case Screen::ClubMix: root = s.club_mix.root; break;
            case Screen::ClubStandings: root = s.club_standings.root; break;
        }
        lv_scr_load(root);
        s.current = model.screen;
    }

    switch (s.current) {
        case Screen::Setup:
            s.setup.update(model.settings, model.live, model.club);
            break;
        case Screen::Live:
            s.live.update(model.live);
            break;
        case Screen::MatchSummary:
            s.summary.update(model.summary);
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
        case Screen::ClubMix:
            s.club_mix.update(model.club);
            break;
        case Screen::ClubStandings:
            s.club_standings.update(model.club);
            break;
    }
}

}  // namespace padel::ui
