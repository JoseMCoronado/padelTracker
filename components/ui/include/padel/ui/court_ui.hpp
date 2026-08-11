#pragma once

#include <array>
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
    std::function<void()> summary_continue;                // summary -> next screen
    std::function<void(Screen)> show_screen;               // navigation request
    std::function<void(TeamId)> begin_pairing;
    std::function<void()> cancel_pairing;
    std::function<void()> confirm_pairing;
    // Drops every remote assigned to the team from the court allow-list. The
    // remote itself clears its credentials the next time it is pressed and
    // the court answers RejectedUnpaired.
    std::function<void(TeamId)> unpair_remote;
    std::function<void(bool resume)> recovery_choice;      // resume vs discard
    std::function<void()> test_beep;                       // diagnostics buzzer test
    // Court backlight 0–100 (Live ORGANIZER menu uses 10–100).
    std::function<void(std::uint8_t percent)> set_brightness;

    // --- Club round ---------------------------------------------------------
    // NEW PLAYER in the picker; host persists to the roster and republishes.
    std::function<void(const std::string& name)> create_player;
    // Start a club round: {teamA[0], teamA[1], teamB[0], teamB[1]}.
    std::function<void(const std::array<ClubPlayer, 4>&, const MatchSettings&)> start_club_round;
    std::function<void()> club_next_set;   // mix screen -> start set 2
    std::function<void()> club_new_round;  // standings -> setup (same roster)
    std::function<void()> club_done;       // standings -> setup, clear club state
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

    // Test/tour hooks: drive state that is otherwise only reachable through
    // touch (the MODE dropdown, the picker modal, the organizer menu).
    void debug_select_preset(int preset_index);
    void debug_open_club_picker(TeamId team);
    void debug_open_organizer_menu(bool open);
    void debug_open_undo_dialog();
    void debug_open_reset_dialog(int step);
    void debug_open_unpair_dialog(TeamId team);
    MatchSettings debug_read_settings() const;

    // Setup-screen edits live inside LVGL widgets; the host reads them back
    // when start is pressed (delivered through start_match callback).

private:
    std::unique_ptr<internal::Screens> screens_;
};

}  // namespace padel::ui
