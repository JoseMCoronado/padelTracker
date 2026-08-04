#pragma once

#include "padel/application/court_service.hpp"
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

// Human label for the scoring mode of a config ("STANDARD / ADV", ...).
std::string mode_label(const domain::MatchConfig& config);

}  // namespace padel::ui
