#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "padel/common/ids.hpp"

namespace padel::ui {

// Which top-level screen is visible (court lifecycle, spec section 12.1).
enum class Screen : std::uint8_t {
    Setup,
    Live,
    MatchComplete,
    Pairing,
    Diagnostics,
    Recovery,
};

// Organizer-editable match settings (setup screen, spec 14.4). Lives in the
// UI host; the scoring preset index maps to domain presets.
struct MatchSettings {
    std::string court_label = "COURT 1";
    std::string team_a_name = "TEAM A";
    std::string team_b_name = "TEAM B";
    std::string players_a{};  // optional "JOSE / MARC"
    std::string players_b{};
    int preset_index = 0;     // see preset_names()
    TeamId first_server = TeamId::A;
};

// Preset labels shown on the setup screen, index-aligned with
// build-side preset selection (spec 14.4).
const std::vector<std::string>& preset_names();

struct TeamPanelModel {
    std::string name;
    std::string players;
    std::string points;        // "0"/"15"/"30"/"40"/"AD" or tiebreak digits
    std::string games;
    std::string sets;
    bool serving = false;
    bool remote_assigned = false;
    bool remote_ok = false;    // assigned and seen recently
};

struct LiveViewModel {
    std::string court_label;
    std::string mode_label;      // "STANDARD / ADV", "GOLDEN POINT", ...
    std::string status_label;    // "LIVE", "PAUSED"
    std::string special_label;   // "DEUCE", "GOLDEN POINT", "TIEBREAK", ""
    TeamPanelModel team_a;
    TeamPanelModel team_b;
    std::string set_history;     // "Set history: 6-4 | current 4-3"
    std::string serving_label;   // "Serving: TEAM A" or ""
    bool radio_ok = true;
    bool storage_fault = false;
    bool conflict = false;       // BOTH TEAMS PRESSED banner
    bool paused = false;
    // Set for one refresh after a point lands: pulse that panel + "+1".
    std::optional<TeamId> point_flash{};
    std::optional<TeamId> undo_preview{};  // team of the point an undo would remove
    std::uint64_t revision = 0;
};

struct CompleteViewModel {
    std::string winner_label;    // "TEAM A WINS"
    std::string final_score;     // "6-4  7-6(5)"
    std::string duration_label;  // "Duration: 52 min" or ""
};

struct PairingViewModel {
    std::string instruction;     // step text
    std::string team_label;      // "Pairing: TEAM A"
    std::string candidate_label; // short device id awaiting confirmation, or ""
    int seconds_left = 0;
    bool awaiting_confirm = false;
};

struct DiagnosticsViewModel {
    // Ordered key/value rows (spec 14.9).
    std::vector<std::pair<std::string, std::string>> rows;
    std::vector<std::string> recent_log_lines;
};

struct RecoveryViewModel {
    std::string message;         // what was recovered / what went wrong
    std::string detail;          // e.g. "Journal: 214 events, tail truncated"
    bool corrupt_tail = false;
};

// Everything the renderer needs for one frame.
struct UiModel {
    Screen screen = Screen::Setup;
    MatchSettings settings{};
    LiveViewModel live{};
    CompleteViewModel complete{};
    PairingViewModel pairing{};
    DiagnosticsViewModel diagnostics{};
    RecoveryViewModel recovery{};
};

}  // namespace padel::ui
