// Interactive terminal scoreboard driving the same command path the court
// unit will use (spec M0 acceptance: full match playable and undoable without
// hardware).

#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "padel/domain/match_engine.hpp"
#include "padel/domain/projection.hpp"

namespace {

using namespace padel;
using namespace padel::domain;

struct Preset {
    MatchConfig config;
    const char* description;
};

const std::map<std::string, Preset>& presets() {
    static const std::map<std::string, Preset> table = {
        {"standard",
         {preset_standard_advantage(), "advantage games, best of 3, tiebreak at 6-6"}},
        {"golden", {preset_standard_golden_point(), "golden point at 40-40, best of 3"}},
        {"club",
         {preset_club_mini_set(), "club mini-set: first to 3 games, golden point, single set"}},
        {"tiebreak-final",
         {preset_match_tiebreak_final(), "best of 3, deciding set = match tiebreak to 10"}},
    };
    return table;
}

const char* lifecycle_label(MatchLifecycle lifecycle) {
    switch (lifecycle) {
        case MatchLifecycle::NotStarted:
            return "NOT STARTED";
        case MatchLifecycle::Active:
            return "LIVE";
        case MatchLifecycle::Paused:
            return "PAUSED";
        case MatchLifecycle::Completed:
            return "COMPLETE";
    }
    return "?";
}

const char* command_error_label(CommandError error) {
    switch (error) {
        case CommandError::MatchNotActive:
            return "match is not active";
        case CommandError::MatchAlreadyStarted:
            return "match already started";
        case CommandError::MatchNotStarted:
            return "match not started (type 'start')";
        case CommandError::MatchCompleted:
            return "match is complete (undo or new/reset)";
        case CommandError::MatchPausedError:
            return "match is paused (type 'resume')";
        case CommandError::NothingToUndo:
            return "nothing to undo";
    }
    return "rejected";
}

void render(const MatchEngine& engine) {
    const DisplayState d = project(engine.state());

    std::string special;
    if (d.is_golden_point) {
        special = "GOLDEN POINT";
    } else if (d.is_deuce) {
        special = "DEUCE";
    } else if (d.is_match_tiebreak) {
        special = "MATCH TIEBREAK";
    } else if (d.is_tiebreak) {
        special = "TIEBREAK";
    }

    std::string history;
    for (const std::string& set : d.set_history) {
        history += (history.empty() ? "" : "  ") + set;
    }

    std::ostringstream out;
    out << "\n";
    out << "  ┌──────────────────────────────────────────────┐\n";
    out << "  │ COURT 1                          " << lifecycle_label(d.lifecycle);
    for (std::size_t i = std::string(lifecycle_label(d.lifecycle)).size(); i < 12; ++i) out << ' ';
    out << "│\n";
    out << "  ├───────────────────────┬──────────────────────┤\n";

    auto row = [&](const std::string& left, const std::string& right) {
        out << "  │ " << left;
        for (std::size_t i = left.size(); i < 22; ++i) out << ' ';
        out << "│ " << right;
        for (std::size_t i = right.size(); i < 21; ++i) out << ' ';
        out << "│\n";
    };

    const std::string serve_a = d.serving_team == TeamId::A ? "  * serving" : "";
    const std::string serve_b = d.serving_team == TeamId::B ? "  * serving" : "";
    row("TEAM A" + serve_a, "TEAM B" + serve_b);
    row("points: " + d.points_a, "points: " + d.points_b);
    row("games:  " + std::to_string(d.games_a), "games:  " + std::to_string(d.games_b));
    row("sets:   " + std::to_string(d.sets_a), "sets:   " + std::to_string(d.sets_b));
    out << "  ├───────────────────────┴──────────────────────┤\n";

    auto wide_row = [&](const std::string& text) {
        out << "  │ " << text;
        for (std::size_t i = text.size(); i < 45; ++i) out << ' ';
        out << "│\n";
    };

    if (!special.empty()) {
        wide_row(">> " + special + " <<");
    }
    if (!history.empty()) {
        wide_row("sets: " + history);
    }
    wide_row("game differential (A-B): " + std::string(d.game_differential > 0 ? "+" : "") +
             std::to_string(d.game_differential));
    if (d.winner) {
        wide_row("WINNER: TEAM " + std::string(*d.winner == TeamId::A ? "A" : "B"));
    }
    out << "  └──────────────────────────────────────────────┘\n";
    std::cout << out.str();
}

void print_help() {
    std::cout << "\ncommands:\n"
              << "  a | b          award a point to team A / team B\n"
              << "  undo           undo last scoring action (any boundary)\n"
              << "  state          redraw the scoreboard\n"
              << "  serve a|b      set serving team\n"
              << "  pause / resume pause or resume the match\n"
              << "  new <preset>   start a new match (see 'presets')\n"
              << "  reset          reset current match (organizer action)\n"
              << "  start          start the match\n"
              << "  presets        list scoring presets\n"
              << "  journal        show event journal size\n"
              << "  help           this help\n"
              << "  quit           exit\n";
}

void print_presets() {
    std::cout << "\npresets:\n";
    for (const auto& [name, preset] : presets()) {
        std::cout << "  " << name;
        for (std::size_t i = name.size(); i < 16; ++i) std::cout << ' ';
        std::cout << preset.description << "\n";
    }
}

void report(const MatchEngine::CommandResult& result) {
    if (!result) {
        std::cout << "  rejected: " << command_error_label(result.error()) << "\n";
    }
}

}  // namespace

int main() {
    std::cout << "Padel Smart Court — score simulator (M0)\n"
              << "type 'help' for commands\n";

    auto engine = std::make_unique<MatchEngine>(preset_standard_advantage());
    engine->handle(StartMatch{TeamId::A});
    std::cout << "\nnew match: standard (type 'presets' for other formats)\n";
    render(*engine);

    std::string line;
    while (std::cout << "\n> " && std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd, arg;
        in >> cmd >> arg;

        if (cmd.empty()) {
            continue;
        } else if (cmd == "a" || cmd == "A") {
            report(engine->handle(AwardPoint{TeamId::A, InputSource::Simulator}));
            render(*engine);
        } else if (cmd == "b" || cmd == "B") {
            report(engine->handle(AwardPoint{TeamId::B, InputSource::Simulator}));
            render(*engine);
        } else if (cmd == "undo") {
            const auto target = engine->next_undo_target();
            const auto result = engine->handle(UndoLastScoringAction{});
            if (result && target) {
                std::cout << "  undid: point for team "
                          << (target->team == TeamId::A ? "A" : "B") << "\n";
            }
            report(result);
            render(*engine);
        } else if (cmd == "state") {
            render(*engine);
        } else if (cmd == "serve") {
            if (arg == "a" || arg == "A") {
                report(engine->handle(SetServingTeam{TeamId::A}));
            } else if (arg == "b" || arg == "B") {
                report(engine->handle(SetServingTeam{TeamId::B}));
            } else {
                std::cout << "  usage: serve a|b\n";
                continue;
            }
            render(*engine);
        } else if (cmd == "pause") {
            report(engine->handle(PauseMatch{}));
            render(*engine);
        } else if (cmd == "resume") {
            report(engine->handle(ResumeMatch{}));
            render(*engine);
        } else if (cmd == "new") {
            const auto it = presets().find(arg.empty() ? "standard" : arg);
            if (it == presets().end()) {
                std::cout << "  unknown preset '" << arg << "'\n";
                print_presets();
                continue;
            }
            engine = std::make_unique<MatchEngine>(it->second.config);
            engine->handle(StartMatch{TeamId::A});
            std::cout << "  new match: " << it->first << "\n";
            render(*engine);
        } else if (cmd == "reset") {
            report(engine->handle(ResetMatch{}));
            engine->handle(StartMatch{TeamId::A});
            std::cout << "  match reset\n";
            render(*engine);
        } else if (cmd == "start") {
            report(engine->handle(StartMatch{TeamId::A}));
            render(*engine);
        } else if (cmd == "presets") {
            print_presets();
        } else if (cmd == "journal") {
            std::cout << "  journal: " << engine->journal().size() << " events, revision "
                      << engine->state().revision << "\n";
        } else if (cmd == "help") {
            print_help();
        } else if (cmd == "quit" || cmd == "q" || cmd == "exit") {
            break;
        } else {
            std::cout << "  unknown command '" << cmd << "' (try 'help')\n";
        }
    }
    return 0;
}
