#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "padel/ui/model.hpp"

namespace padel::ui {

// Host-provided actions. The UI never talks to CourtService directly: every
// touch converges on the same command path as remotes and wired buttons
// (spec section 15). Screen navigation is owned by the host: buttons request
// a screen via show_screen and the host renders the new UiModel.
struct UiCallbacks {
    std::function<void(TeamId)> award_point;               // touchscreen admin +1
    std::function<void()> undo_confirmed;                  // after on-screen preview
    std::function<void()> toggle_pause;
    std::function<void(std::optional<TeamId>)> resolve_conflict;
    std::function<void(const MatchSettings&)> start_match;
    std::function<void()> reset_confirmed;                 // after two-step confirm
    std::function<void()> new_match;                       // complete -> setup
    std::function<void(Screen)> show_screen;               // navigation request
    std::function<void(TeamId)> begin_pairing;
    std::function<void()> cancel_pairing;
    std::function<void()> confirm_pairing;
    std::function<void(bool resume)> recovery_choice;      // resume vs discard
    std::function<void()> test_beep;                       // diagnostics buzzer test
};

namespace internal {
struct Screens;
}

// Applies the dark default theme with our palette to the active display.
// Hosts call this once after registering their display driver, before
// CourtUi::create().
void init_theme();

// Owns all LVGL screens. create() builds them once; render() switches to the
// screen in the model and refreshes its widgets (cheap diffing per label).
// The host drives render() from its LVGL-safe context after state changes
// and on a coarse timer.
class CourtUi {
public:
    CourtUi();
    ~CourtUi();

    void create(UiCallbacks callbacks);
    void render(const UiModel& model);

    // Setup-screen edits live inside LVGL widgets; the host reads them back
    // when start is pressed (delivered through start_match callback).

private:
    std::unique_ptr<internal::Screens> screens_;
};

}  // namespace padel::ui
