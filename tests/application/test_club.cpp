// Roster (seed list, search filter, guests) and ClubController (round
// orchestration over completed mini-set states, results log, forbidden
// pair).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "padel/application/club_controller.hpp"
#include "padel/application/roster.hpp"
#include "padel/domain/club_round.hpp"

using namespace padel;
using application::ClubController;
using application::Player;
using application::PlayerRoster;

namespace {

class MemoryRosterStore : public application::IRosterStore {
public:
    std::vector<Player> load() override { return stored; }
    bool save(const std::vector<Player>& players) override {
        stored = players;
        ++saves;
        return true;
    }
    std::vector<Player> stored;
    int saves = 0;
};

class MemoryResultsLog : public application::IResultsLog {
public:
    bool append(const application::RoundResult& result) override {
        rows.push_back(result);
        return true;
    }
    std::vector<application::RoundResult> rows;
};

class FakeClock : public application::IClock {
public:
    std::uint64_t now_ms() const override { return 777; }
};

// Builds a completed club mini-set MatchState (single set, first to 3).
domain::MatchState completed_set(TeamId winner, std::uint8_t loser_games) {
    domain::MatchState state{};
    state.lifecycle = domain::MatchLifecycle::Completed;
    state.winner = winner;
    state.completed_set_count = 1;
    state.completed_sets[0].games_a = winner == TeamId::A ? 3 : loser_games;
    state.completed_sets[0].games_b = winner == TeamId::B ? 3 : loser_games;
    return state;
}

// The regulars the round tests below pick from. Deliberately their own list
// rather than config/players.txt: who plays on a Tuesday is not a fact these
// tests should break on.
const std::vector<std::string>& regulars() {
    static const std::vector<std::string> names = {
        "Jose",    "Zoe",    "William", "Szewei",  "Ruxandra", "Lewis",
        "Luigi",   "Raymond", "Paulina", "Vineet", "Louis",    "Adrien"};
    return names;
}

Player must_find(const PlayerRoster& roster, const char* name) {
    for (const Player& player : roster.players()) {
        if (player.name == name) {
            return player;
        }
    }
    FAIL("missing seeded player: " << name);
    return {};
}

std::array<Player, 4> four_players(PlayerRoster& roster) {
    return {must_find(roster, "Adrien"), must_find(roster, "Lewis"),
            must_find(roster, "Louis"), must_find(roster, "Luigi")};
}

}  // namespace

TEST_CASE("club list parses names, skipping comments and blanks") {
    const auto names = application::parse_club_list(
        "# the club list\n"
        "\n"
        "Jose\n"
        "  Zoe  \n"
        "Marcelo # crowned last week\n"
        "   \n"
        "jose\n"          // same player, different casing
        "Sonny");         // no trailing newline
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "Jose");
    CHECK(names[1] == "Zoe");
    CHECK(names[2] == "Marcelo");
    CHECK(names[3] == "Sonny");
}

TEST_CASE("club list fills an empty roster and persists it once") {
    MemoryRosterStore store;
    PlayerRoster roster{store};
    CHECK(roster.players().empty());  // the list is the only source of regulars
    CHECK(store.saves == 0);

    const auto sync = roster.apply_club_list({"Zoe", "Jose", "Adrien"});
    CHECK(sync.applied);
    CHECK(sync.added == 3);
    CHECK(sync.removed == 0);
    CHECK(store.saves == 1);
    REQUIRE(roster.players().size() == 3);
    // Sorted by name for stable picker grids, with unique ids.
    CHECK(roster.players()[0].name == "Adrien");
    CHECK(roster.players()[2].name == "Zoe");
    std::vector<std::uint32_t> ids;
    for (const Player& player : roster.players()) {
        CHECK(player.from_club_list);
        CHECK_FALSE(player.guest);
        ids.push_back(player.id);
    }
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

TEST_CASE("club list is authoritative for its own names, but not for courtside additions") {
    MemoryRosterStore store;
    PlayerRoster roster{store};
    roster.apply_club_list({"Zoe", "Jose"});
    const std::uint32_t zoe_id = roster.players()[1].id;
    REQUIRE(roster.players()[1].name == "Zoe");

    // A walk-up typed in on the court.
    const auto walk_up = roster.add_player("Marcelo");
    REQUIRE(walk_up.has_value());
    CHECK_FALSE(walk_up->from_club_list);

    // Jose leaves the club, Sarah joins it.
    const auto sync = roster.apply_club_list({"Zoe", "Sarah"});
    CHECK(sync.added == 1);
    CHECK(sync.removed == 1);
    std::vector<std::string> names;
    for (const Player& player : roster.players()) {
        names.push_back(player.name);
    }
    CHECK(names == std::vector<std::string>{"Marcelo", "Sarah", "Zoe"});
    // Zoe stayed on the list, so she keeps the id her results are logged under.
    CHECK(roster.find(zoe_id).has_value());
    CHECK(roster.find(zoe_id)->name == "Zoe");
}

TEST_CASE("a name added to the club list adopts the player typed in courtside") {
    MemoryRosterStore store;
    PlayerRoster roster{store};
    const auto walk_up = roster.add_player("Marcelo");
    REQUIRE(walk_up.has_value());

    // Same player, now a regular: no duplicate row, and the id is kept.
    const auto sync = roster.apply_club_list({"marcelo"});
    CHECK(sync.added == 0);
    CHECK(sync.removed == 0);
    REQUIRE(roster.players().size() == 1);
    CHECK(roster.players()[0].id == walk_up->id);
    CHECK(roster.players()[0].name == "Marcelo");  // the courtside spelling stands
    CHECK(roster.players()[0].from_club_list);

    // And now the list can drop them.
    CHECK(roster.apply_club_list({"Zoe"}).removed == 1);
}

TEST_CASE("an empty club list never wipes the roster") {
    MemoryRosterStore store;
    store.stored = {{7, "Maria", false, true}};
    PlayerRoster roster{store};
    REQUIRE(roster.players().size() == 1);

    // A missing file, a failed read or a truncated one all arrive here as an
    // empty list; obeying it would delete the whole club.
    const auto sync = roster.apply_club_list({});
    CHECK_FALSE(sync.applied);
    CHECK(sync.added == 0);
    CHECK(sync.removed == 0);
    CHECK(roster.players().size() == 1);
    CHECK(store.saves == 0);
}

TEST_CASE("an unchanged club list does not rewrite the roster file") {
    MemoryRosterStore store;
    PlayerRoster roster{store};
    roster.apply_club_list({"Zoe", "Jose"});
    REQUIRE(store.saves == 1);

    const auto sync = roster.apply_club_list({"Jose", "Zoe"});  // same names, reordered
    CHECK(sync.applied);
    CHECK(sync.added == 0);
    CHECK(sync.removed == 0);
    CHECK(store.saves == 1);  // every boot would otherwise burn a flash write
}

TEST_CASE("an existing store is loaded verbatim, without a write") {
    MemoryRosterStore store;
    store.stored = {{7, "Maria", false, false}};
    PlayerRoster roster{store};
    REQUIRE(roster.players().size() == 1);
    CHECK(roster.players()[0].name == "Maria");
    CHECK(roster.players()[0].id == 7);  // ids outlive everything; results log them
    CHECK_FALSE(roster.players()[0].from_club_list);
    CHECK(store.saves == 0);
}

TEST_CASE("search filter is case-insensitive substring") {
    MemoryRosterStore store;
    PlayerRoster roster(store);
    roster.apply_club_list(regulars());
    roster.add_player("John");

    const auto jo = roster.filtered("jo");
    REQUIRE(jo.size() == 2);  // John, Jose
    CHECK(jo[0].name == "John");
    CHECK(jo[1].name == "Jose");

    const auto lou = roster.filtered("lou");
    REQUIRE(lou.size() == 1);
    CHECK(lou[0].name == "Louis");

    CHECK(roster.filtered("luigi").size() == 1);
    CHECK(roster.filtered("LEWIS").size() == 1);
    CHECK(roster.filtered("ZOE").size() == 1);
    CHECK(roster.filtered("  ").size() == roster.players().size());
    CHECK(roster.filtered("xyz").empty());
}

TEST_CASE("add_player trims, rejects duplicates and empties, persists") {
    MemoryRosterStore store;
    PlayerRoster roster(store);
    roster.apply_club_list(regulars());
    const int saves_after_seed = store.saves;

    const auto maria = roster.add_player("  Maria  ");
    REQUIRE(maria.has_value());
    CHECK(maria->name == "Maria");
    CHECK(store.saves == saves_after_seed + 1);

    CHECK_FALSE(roster.add_player("maria").has_value());  // duplicate, case-insensitive
    CHECK_FALSE(roster.add_player("   ").has_value());
    CHECK(roster.find(maria->id)->name == "Maria");
}

TEST_CASE("guests are numbered, findable, and never persisted") {
    MemoryRosterStore store;
    PlayerRoster roster(store);
    roster.apply_club_list(regulars());
    const int saves_before = store.saves;

    const Player g1 = roster.make_guest();
    const Player g2 = roster.make_guest();
    CHECK(g1.name == "GUEST");
    CHECK(g2.name == "GUEST 2");
    CHECK(g1.guest);
    CHECK(store.saves == saves_before);
    CHECK(roster.find(g1.id)->guest);
    for (const Player& player : roster.players()) {
        CHECK_FALSE(player.guest);
    }
}

TEST_CASE("full round: labels, mix, standings, results log") {
    MemoryRosterStore store;
    MemoryResultsLog log;
    FakeClock clock;
    PlayerRoster roster(store);
    roster.apply_club_list(regulars());
    ClubController controller(log, clock);

    const auto players = four_players(roster);  // Adrien&Lewis vs Louis&Luigi
    REQUIRE_FALSE(controller.start_round(players, 0).has_value());
    CHECK(controller.set_number() == 1);
    CHECK(controller.current_set_teams().team_a == "ADRIEN & LEWIS");
    CHECK(controller.current_set_teams().team_b == "LOUIS & LUIGI");

    // Set 1: Adrien&Lewis win 3-0. Mix: winners split -> ADRIEN & LOUIS vs LEWIS & LUIGI.
    controller.on_set_complete(completed_set(TeamId::A, 0));
    CHECK(controller.stage() == domain::ClubStage::Set2);
    CHECK(controller.set_number() == 2);
    CHECK(controller.current_set_teams().team_a == "ADRIEN & LOUIS");
    CHECK(controller.current_set_teams().team_b == "LEWIS & LUIGI");

    // Set 2: Adrien&Louis win 3-2 -> Adrien 2 wins; Lewis +3-1=+2 beats Louis -3+1=-2.
    controller.on_set_complete(completed_set(TeamId::A, 2));
    REQUIRE(controller.stage() == domain::ClubStage::Complete);

    const auto standings = controller.standings();
    REQUIRE(standings.size() == 4);
    CHECK(standings[0].player.name == "Adrien");
    CHECK(standings[0].wins == 2);
    CHECK(standings[0].top2);
    CHECK(standings[1].player.name == "Lewis");
    CHECK(standings[1].top2);
    CHECK_FALSE(standings[2].top2);
    CHECK(controller.coin_flip_announcement().empty());

    // The log is written when the organizer closes the round, not the moment
    // set 2 lands: until then an undo can still reopen it.
    CHECK(log.rows.empty());
    controller.finish_round();

    REQUIRE(log.rows.size() == 4);
    CHECK(log.rows[0].player_name == "Adrien");
    CHECK(log.rows[0].top2);
    CHECK(log.rows[0].timestamp_ms == 777);
    CHECK(log.rows[3].wins == 0);
}

TEST_CASE("recorded sets keep the names that played them") {
    MemoryRosterStore store;
    MemoryResultsLog log;
    FakeClock clock;
    PlayerRoster roster(store);
    roster.apply_club_list(regulars());
    ClubController controller(log, clock);

    REQUIRE_FALSE(controller.start_round(four_players(roster), 0).has_value());
    CHECK(controller.recorded_sets().empty());

    controller.on_set_complete(completed_set(TeamId::B, 1));  // LOUIS & LUIGI 3-1
    auto played = controller.recorded_sets();
    REQUIRE(played.size() == 1);
    CHECK(played[0].team_a == "ADRIEN & LEWIS");
    CHECK(played[0].team_b == "LOUIS & LUIGI");
    CHECK(played[0].games_a == 1);
    CHECK(played[0].games_b == 3);
    CHECK(played[0].winner == TeamId::B);

    // The mix swaps partners, so set 2 reads back with its own pairing.
    controller.on_set_complete(completed_set(TeamId::A, 2));
    played = controller.recorded_sets();
    REQUIRE(played.size() == 2);
    CHECK(played[0].team_a == "ADRIEN & LEWIS");
    CHECK(played[1].team_a == "LOUIS & ADRIEN");
    CHECK(played[1].team_b == "LUIGI & LEWIS");
    CHECK(played[1].games_a == 3);
    CHECK(played[1].games_b == 2);

    REQUIRE(controller.undo_last_set());
    CHECK(controller.recorded_sets().size() == 1);
}

TEST_CASE("an undo reopens set 2 and the results log is still written once") {
    MemoryRosterStore store;
    MemoryResultsLog log;
    FakeClock clock;
    PlayerRoster roster(store);
    roster.apply_club_list(regulars());
    ClubController controller(log, clock);

    REQUIRE_FALSE(controller.start_round(four_players(roster), 0).has_value());
    controller.on_set_complete(completed_set(TeamId::A, 0));
    controller.on_set_complete(completed_set(TeamId::A, 2));
    REQUIRE(controller.stage() == domain::ClubStage::Complete);

    // The court display undoes the winning point: set 2 goes back into play.
    REQUIRE(controller.undo_last_set());
    CHECK(controller.stage() == domain::ClubStage::Set2);
    CHECK(controller.set_number() == 2);
    CHECK(controller.last_set_summary() == "ADRIEN & LEWIS took set 1 (3-0)");
    CHECK(log.rows.empty());

    // Replayed to a different result; only the corrected round is logged.
    controller.on_set_complete(completed_set(TeamId::B, 2));
    REQUIRE(controller.stage() == domain::ClubStage::Complete);
    controller.finish_round();
    CHECK(log.rows.size() == 4);
}

TEST_CASE("undo walks back through set 1 and stops at the start of the round") {
    MemoryRosterStore store;
    MemoryResultsLog log;
    FakeClock clock;
    PlayerRoster roster(store);
    roster.apply_club_list(regulars());
    ClubController controller(log, clock);

    REQUIRE_FALSE(controller.start_round(four_players(roster), 0).has_value());
    CHECK_FALSE(controller.undo_last_set());  // nothing recorded yet

    controller.on_set_complete(completed_set(TeamId::A, 0));
    REQUIRE(controller.undo_last_set());
    CHECK(controller.stage() == domain::ClubStage::Set1);
    CHECK(controller.last_set_summary().empty());
    CHECK(controller.current_set_teams().team_a == "ADRIEN & LEWIS");
}

TEST_CASE("crowned players cannot be teammates even when they never were Top 2") {
    MemoryRosterStore store;
    MemoryResultsLog log;
    FakeClock clock;
    PlayerRoster roster(store);
    roster.apply_club_list(regulars());
    ClubController controller(log, clock);

    const auto players = four_players(roster);  // Adrien&Lewis vs Louis&Luigi
    const ClubController::ForbiddenPair crown{players[0].id, players[1].id};
    CHECK(controller.start_round(players, 0, {crown}) ==
          ClubController::StartError::ForbiddenPair);

    // Split across the teams and the round starts.
    const std::array<Player, 4> split{players[0], players[2], players[1], players[3]};
    CHECK_FALSE(controller.start_round(split, 0, {crown}).has_value());
}

TEST_CASE("tied differential announces the coin flip") {
    MemoryRosterStore store;
    MemoryResultsLog log;
    FakeClock clock;
    PlayerRoster roster(store);
    roster.apply_club_list(regulars());
    ClubController controller(log, clock);

    REQUIRE_FALSE(controller.start_round(four_players(roster), 0).has_value());
    // Both sets 3-2 with alternating winners -> the two 1-win players tie.
    controller.on_set_complete(completed_set(TeamId::A, 2));
    controller.on_set_complete(completed_set(TeamId::B, 2));
    REQUIRE(controller.stage() == domain::ClubStage::Complete);

    const std::string announcement = controller.coin_flip_announcement();
    REQUIRE_FALSE(announcement.empty());
    CHECK(announcement.rfind("COIN FLIP: ", 0) == 0);
    CHECK(announcement.find("takes the last TOP 2 spot") != std::string::npos);

    controller.finish_round();
    int flip_rows = 0;
    for (const auto& row : log.rows) {
        flip_rows += row.decided_by_coin_flip ? 1 : 0;
    }
    CHECK(flip_rows == 1);
}

TEST_CASE("previous top2 cannot be teammates but may play on opposite sides") {
    MemoryRosterStore store;
    MemoryResultsLog log;
    FakeClock clock;
    PlayerRoster roster(store);
    roster.apply_club_list(regulars());
    ClubController controller(log, clock);

    const auto players = four_players(roster);
    REQUIRE_FALSE(controller.start_round(players, 0).has_value());
    controller.on_set_complete(completed_set(TeamId::A, 0));
    controller.on_set_complete(completed_set(TeamId::A, 2));  // top2 = Adrien, Lewis
    controller.finish_round();
    REQUIRE(controller.forbidden_pair_ids().has_value());

    // Same team A pair (Adrien & Lewis) again -> rejected.
    const auto error = controller.start_round(players, 0);
    REQUIRE(error.has_value());
    CHECK(*error == ClubController::StartError::ForbiddenPair);

    // Split them across teams on the same court -> allowed.
    const std::array<Player, 4> reshuffled = {players[0], players[2], players[1], players[3]};
    CHECK_FALSE(controller.start_round(reshuffled, 0).has_value());
}

TEST_CASE("abandoning an incomplete round keeps the prior top2 teammate guard") {
    MemoryRosterStore store;
    MemoryResultsLog log;
    FakeClock clock;
    PlayerRoster roster(store);
    roster.apply_club_list(regulars());
    ClubController controller(log, clock);

    const auto players = four_players(roster);
    REQUIRE_FALSE(controller.start_round(players, 0).has_value());
    controller.on_set_complete(completed_set(TeamId::A, 0));
    controller.on_set_complete(completed_set(TeamId::A, 2));  // top2 = Adrien, Lewis
    controller.finish_round();
    const auto forbidden = controller.forbidden_pair_ids();
    REQUIRE(forbidden.has_value());

    // Start a new round, then abandon mid-set (RESET / NEW MATCH path).
    const std::array<Player, 4> next = {players[0], players[2], players[1], players[3]};
    REQUIRE_FALSE(controller.start_round(next, 1).has_value());
    controller.finish_round();  // incomplete -> must not clear prior Top 2
    CHECK(controller.forbidden_pair_ids() == forbidden);

    const auto as_teammates = controller.start_round(players, 2);
    REQUIRE(as_teammates.has_value());
    CHECK(*as_teammates == ClubController::StartError::ForbiddenPair);
    CHECK_FALSE(controller.start_round(next, 3).has_value());
}

TEST_CASE("duplicate player and double start are rejected") {
    MemoryRosterStore store;
    MemoryResultsLog log;
    FakeClock clock;
    PlayerRoster roster(store);
    roster.apply_club_list(regulars());
    ClubController controller(log, clock);

    auto players = four_players(roster);
    players[3] = players[0];
    const auto duplicate = controller.start_round(players, 0);
    REQUIRE(duplicate.has_value());
    CHECK(*duplicate == ClubController::StartError::DuplicatePlayer);

    REQUIRE_FALSE(controller.start_round(four_players(roster), 0).has_value());
    const auto again = controller.start_round(four_players(roster), 0);
    REQUIRE(again.has_value());
    CHECK(*again == ClubController::StartError::RoundActive);
}
