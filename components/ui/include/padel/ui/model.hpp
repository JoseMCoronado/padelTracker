#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "padel/common/ids.hpp"

namespace padel::ui {

// Which top-level screen is visible (court lifecycle, spec section 12.1).
enum class Screen : std::uint8_t {
    Setup,
    Live,
    MatchSummary,   // read the match back before the flow moves on
    MatchComplete,
    Pairing,
    Diagnostics,
    Recovery,
    ClubMix,        // between club mini-sets: announce the mixed teams
    ClubStandings,  // after set 2: standings, top2/bottom2, coin flip
};

// Screens the flow reaches only after a match has finished. An undo that
// takes the winning point back has to walk all of them back to Live.
constexpr bool is_post_match_screen(Screen screen) {
    return screen == Screen::MatchSummary || screen == Screen::MatchComplete ||
           screen == Screen::ClubMix || screen == Screen::ClubStandings;
}

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

// Mode labels shown on the setup screen (MODE dropdown), index-aligned with
// build-side preset selection (spec 14.4). "Club round" is a mode, not just
// a scoring preset: it enables the player pickers and the round flow.
const std::vector<std::string>& preset_names();

// Index of the club round entry in preset_names(); its scoring config is
// the club mini-set (first to 3).
inline constexpr int kClubRoundPreset = 4;

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

// One set column of the broadcast-style scoreboard, laid out like the pro
// tour overlays: a games digit per team, the loser's tiebreak points in
// brackets, the set in progress highlighted.
struct ScoreColumn {
    std::string games_a;
    std::string games_b;
    std::string tiebreak_a;   // "(5)" or ""
    std::string tiebreak_b;
    bool current = false;
    std::optional<TeamId> won{};
};

struct ScoreboardModel {
    std::string name_a;                // short pair label, e.g. "TRI/BRE"
    std::string name_b;
    std::vector<ScoreColumn> columns;  // completed sets, set in progress last
    std::optional<TeamId> serving{};
};

// Club round: the mini-sets already played, each keeping its own pairing
// because the mix swaps partners between sets. Drawn as extra scoreboard
// blocks to the left of the board for the set on screen.
using PriorScoreboards = std::vector<ScoreboardModel>;

struct LiveViewModel {
    std::string court_label;
    std::string mode_label;      // "STANDARD / ADV", "GOLDEN POINT", ...
    std::string status_label;    // "LIVE", "PAUSED"
    std::string special_label;   // "DEUCE", "GOLDEN POINT", "TIEBREAK", ""
    TeamPanelModel team_a;
    TeamPanelModel team_b;
    ScoreboardModel scoreboard;
    PriorScoreboards prior_scoreboards;
    bool radio_ok = true;
    bool storage_fault = false;
    bool conflict = false;       // BOTH TEAMS PRESSED banner
    bool paused = false;
    // Court Li-ion SoC from the Waveshare ADC; empty when no cell / unknown.
    // Filtered and slew-limited upstream, so it moves a point at a time.
    std::optional<std::uint8_t> battery_percent{};
    // Low warning, latched with hysteresis upstream so it cannot flicker on
    // the threshold the way a bare percent comparison would.
    bool battery_low = false;
    // Pre-formatted runtime estimate ("~2h 15m"); empty means show nothing.
    std::string battery_runtime;
    // Last applied backlight brightness (ORGANIZER menu slider, 10–100).
    std::uint8_t brightness_percent = 100;
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

// Read the match back before the flow moves on (club mix, standings, or the
// ordinary match-complete screen).
struct SummaryViewModel {
    std::string title;           // "SET 1 COMPLETE" / "MATCH COMPLETE"
    std::string winner_label;    // "JOSE & ZOE WIN"
    ScoreboardModel scoreboard;  // final set-by-set, same widget as live
    PriorScoreboards prior_scoreboards;
    // Ordered label/value stat rows: duration, points won, longest streak.
    std::vector<std::pair<std::string, std::string>> stats;
    std::string continue_label = "CONTINUE";
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

// --- Club round --------------------------------------------------------------

// A pickable player tile. Members come from the persisted roster; guests are
// minted inside the picker (sentinel ids, never persisted).
struct ClubPlayer {
    std::uint32_t id = 0;
    std::string name;
    bool guest = false;
    // Crown group, 0 = none. Two players wearing the same crown were a Top 2
    // together and can never be teammates this round (rotation sheet rule).
    // A court needs two groups: the pair that stayed and the pair that came
    // up from the court below.
    std::uint8_t crown = 0;
};

inline constexpr std::uint8_t kMaxCrownGroups = 2;

struct ClubStandingRowModel {
    std::string rank;    // "1".."4"
    std::string name;
    std::string record;  // "2 WINS  +4"
    bool top2 = false;
    bool coin = false;   // this spot came from the automatic coin flip
};

struct ClubViewModel {
    std::vector<ClubPlayer> roster;   // members for the picker grid
    std::string setup_hint;           // forbidden-pair / validation message, or ""

    // Mix screen (after set 1).
    std::string mix_detail;           // "JOSE & ZOE took set 1 (3-1)"
    std::string mix_team_a;           // set 2 pairing
    std::string mix_team_b;

    // Standings screen (after set 2).
    std::vector<ClubStandingRowModel> standings;
    std::string coin_announcement;    // "" when the differential decided it

    // NEW ROUND suggestion: Setup applies when suggestion_seq advances.
    // Only the Top 2 — one on Team A, one on Team B; partners left empty.
    std::vector<ClubPlayer> suggested_a;
    std::vector<ClubPlayer> suggested_b;
    std::uint32_t suggestion_seq = 0;
};

// Everything the renderer needs for one frame.
struct UiModel {
    Screen screen = Screen::Setup;
    MatchSettings settings{};
    LiveViewModel live{};
    SummaryViewModel summary{};
    CompleteViewModel complete{};
    PairingViewModel pairing{};
    DiagnosticsViewModel diagnostics{};
    RecoveryViewModel recovery{};
    ClubViewModel club{};
};

}  // namespace padel::ui
