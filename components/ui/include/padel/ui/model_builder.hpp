#pragma once

#include "padel/application/club_controller.hpp"
#include "padel/application/court_service.hpp"
#include "padel/application/roster.hpp"
#include "padel/domain/types.hpp"
#include "padel/ui/model.hpp"

namespace padel::ui {

// Maps a setup-screen preset index to a domain MatchConfig.
domain::MatchConfig preset_config(int preset_index);

// Builds the live-match view model from the authoritative service state
// (spec invariant 3.2.7: UI state is projected, never maintained
// independently). now_ms is used for remote liveness.
LiveViewModel build_live_model(const application::CourtService& service,
                               const MatchSettings& settings,
                               std::uint64_t now_ms);

CompleteViewModel build_complete_model(const application::CourtService& service,
                                       const MatchSettings& settings,
                                       std::uint64_t match_duration_ms);

// Post-match read-back shown before the flow continues. title_override names
// the club mini-set that just finished ("SET 1 COMPLETE"); pass "" for an
// ordinary match.
SummaryViewModel build_summary_model(const application::CourtService& service,
                                     const MatchSettings& settings,
                                     std::uint64_t match_duration_ms,
                                     const std::string& title_override = {},
                                     const std::string& continue_label = "CONTINUE");

// Human label for the scoring mode of a config ("STANDARD / ADV", ...).
std::string mode_label(const domain::MatchConfig& config);

// Club view model: roster tiles for the picker plus, when a round is
// active, the mix pairing / standings projected from the controller.
// setup_hint carries a validation message (e.g. forbidden pair), or "".
ClubViewModel build_club_model(const application::PlayerRoster& roster,
                               const application::ClubController& controller,
                               const std::string& setup_hint);

// Groups the crowns the organizer set in the picker into the pairs the club
// round must keep apart. A crown worn by only one of the four picks is
// ignored: the partner it refers to is playing on another court.
std::vector<application::ClubController::ForbiddenPair> crown_pairs(
    const std::array<ClubPlayer, 4>& picked);

// While a round is Complete (before finish_round): place Top 2 alone on
// opposite teams (rank-1 -> Team A, rank-2 -> Team B). Partner slots stay
// empty for the organizer. Returns false if standings are not ready.
bool suggest_next_round_picks(const application::ClubController& controller,
                              std::vector<ClubPlayer>& out_a,
                              std::vector<ClubPlayer>& out_b);

}  // namespace padel::ui
