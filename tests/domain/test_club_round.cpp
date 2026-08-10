// Club round logic against the exact examples on the club rotation sheets
// (rotation_examples/*.png). Slots: 0,1 = Team A picks; 2,3 = Team B picks.

#include <catch2/catch_test_macros.hpp>

#include "padel/domain/club_round.hpp"

using namespace padel;
using domain::ClubPairing;
using domain::ClubRound;
using domain::ClubStage;

namespace {

bool pair_is(const std::array<std::uint8_t, 2>& pair, std::uint8_t x, std::uint8_t y) {
    return (pair[0] == x && pair[1] == y) || (pair[0] == y && pair[1] == x);
}

}  // namespace

TEST_CASE("set 1 pairing is team A picks vs team B picks") {
    ClubRound round;
    REQUIRE(round.stage() == ClubStage::Set1);
    const ClubPairing pairing = round.current_pairing();
    CHECK(pair_is(pairing.team_a, 0, 1));
    CHECK(pair_is(pairing.team_b, 2, 3));
}

TEST_CASE("set 2 mix: winners split and each takes a loser") {
    // Sheet round 1, court 2: A&B beat C&D, set 2 is A&C vs B&D.
    ClubRound round;
    round.record_set_result(TeamId::A, 3, 0);
    REQUIRE(round.stage() == ClubStage::Set2);
    const ClubPairing mix = round.current_pairing();
    CHECK(pair_is(mix.team_a, 0, 2));  // A & C
    CHECK(pair_is(mix.team_b, 1, 3));  // B & D
}

TEST_CASE("set 2 mix when team B wins set 1 still splits the winners") {
    ClubRound round;
    round.record_set_result(TeamId::B, 3, 1);
    const ClubPairing mix = round.current_pairing();
    // Winners C,D split; C takes A, D takes B -> {2,0} vs {3,1}.
    CHECK(pair_is(mix.team_a, 2, 0));
    CHECK(pair_is(mix.team_b, 3, 1));
}

TEST_CASE("sheet round 1 court 2: A&B 3-0, then A&C 3-2 -> top2 A,B") {
    // A=0 B=1 C=2 D=3.
    ClubRound round;
    round.record_set_result(TeamId::A, 3, 0);  // A&B over C&D
    round.record_set_result(TeamId::A, 3, 2);  // A&C over B&D
    REQUIRE(round.stage() == ClubStage::Complete);

    const auto standings = round.standings();
    // A: 2 wins, +4. B: 1 win, +3-1=+2. C: 1 win, -3+1=-2. D: 0 wins, -4.
    CHECK(standings[0].slot == 0);
    CHECK(standings[0].wins == 2);
    CHECK(standings[0].differential == 4);
    CHECK(standings[1].slot == 1);
    CHECK(standings[1].differential == 2);
    CHECK(standings[2].slot == 2);
    CHECK(standings[3].slot == 3);

    CHECK(pair_is(round.top2(), 0, 1));     // sheet: TOP 2 = A & B
    CHECK(pair_is(round.bottom2(), 2, 3));  // sheet: BOTTOM 2 = C & D
    CHECK_FALSE(round.decided_by_coin_flip());
    CHECK(pair_is(round.forbidden_pair(), 0, 1));
}

TEST_CASE("sheet round 1 court 3: G&H 3-2, then E&G 3-1 -> top2 G,E") {
    // E=0 F=1 G=2 H=3. Set 1: G&H (team B) win 3-2. Set 2 mix: G&E vs H&F,
    // E&G win 3-1.
    ClubRound round;
    round.record_set_result(TeamId::B, 3, 2);
    const ClubPairing mix = round.current_pairing();
    CHECK(pair_is(mix.team_a, 2, 0));  // G & E
    CHECK(pair_is(mix.team_b, 3, 1));  // H & F
    round.record_set_result(TeamId::A, 3, 1);

    // G: 2 wins. E: 1 win, -1+2=+1. H: 1 win, +1-2=-1. F: 0 wins.
    CHECK(pair_is(round.top2(), 2, 0));     // sheet: TOP 2 = G & E
    CHECK(pair_is(round.bottom2(), 3, 1));  // sheet: BOTTOM 2 = H & F
    CHECK_FALSE(round.decided_by_coin_flip());
}

TEST_CASE("sheet round 2 court 2: the mix avoids handing the Top 2 back to each other") {
    // A and B stayed on this court as last round's Top 2, so they can never
    // partner. Slots: A=0, G=1 (team A picks), B=2, E=3 (team B picks).
    domain::ClubForbiddenPairs barred{};
    barred.add(0, 2);  // A & B
    ClubRound round(0, barred);

    round.record_set_result(TeamId::A, 3, 2);  // A&G take set 1
    const ClubPairing mix = round.current_pairing();
    // The default split would be A&B vs G&E, exactly the pair the sheet
    // forbids, so the round takes the other split: A&E vs B&G.
    CHECK(pair_is(mix.team_a, 0, 3));
    CHECK(pair_is(mix.team_b, 1, 2));
}

TEST_CASE("two barred pairs still leave the mix a legal split") {
    // A&B stayed, G&E came up together: neither pair may partner.
    domain::ClubForbiddenPairs barred{};
    barred.add(0, 2);  // A & B
    barred.add(1, 3);  // G & E
    ClubRound round(0, barred);

    round.record_set_result(TeamId::A, 3, 2);
    const ClubPairing mix = round.current_pairing();
    CHECK(pair_is(mix.team_a, 0, 3));  // A & E
    CHECK(pair_is(mix.team_b, 1, 2));  // G & B
}

TEST_CASE("undo_last_set_result rewinds one set at a time") {
    ClubRound round;
    CHECK_FALSE(round.undo_last_set_result());  // nothing recorded yet

    round.record_set_result(TeamId::A, 3, 0);
    round.record_set_result(TeamId::A, 3, 2);
    REQUIRE(round.stage() == ClubStage::Complete);

    REQUIRE(round.undo_last_set_result());
    CHECK(round.stage() == ClubStage::Set2);
    // Set 1 survived, so the mix is unchanged and set 2 can be replayed.
    const ClubPairing mix = round.current_pairing();
    CHECK(pair_is(mix.team_a, 0, 2));
    CHECK(pair_is(mix.team_b, 1, 3));

    REQUIRE(round.undo_last_set_result());
    CHECK(round.stage() == ClubStage::Set1);
    CHECK_FALSE(round.undo_last_set_result());
}

TEST_CASE("a replayed set 2 produces the standings of the replay, not the undone set") {
    ClubRound round;
    round.record_set_result(TeamId::A, 3, 0);
    round.record_set_result(TeamId::A, 3, 2);  // A&C over B&D
    REQUIRE(pair_is(round.top2(), 0, 1));

    REQUIRE(round.undo_last_set_result());
    round.record_set_result(TeamId::B, 3, 0);  // B&D over A&C instead
    REQUIRE(round.stage() == ClubStage::Complete);
    // B now has 2 wins; A dropped to one.
    CHECK(round.standings()[0].slot == 1);
    CHECK(round.standings()[0].wins == 2);
}

TEST_CASE("tied differential resolved by coin flip") {
    // Set 1: {0,1} win 3-2. Set 2 mix: {0,2} vs {1,3}, and {1,3} win 3-2.
    // Wins: slot 1 = 2. Slot 0: +1-1=0, 1 win. Slot 3: -1+1=0, 1 win.
    // Slots 0 and 3 tie on differential -> automatic coin flip.
    // Wins: G=2. A: +1-1=0, 1 win. E: -1+1=0, 1 win. B: 0 wins.
    // A and E tie on differential -> automatic coin flip.
    ClubRound heads(0);  // seed & 1 == 0: first-ranked tied slot keeps the spot
    heads.record_set_result(TeamId::A, 3, 2);
    heads.record_set_result(TeamId::B, 3, 2);
    REQUIRE(heads.decided_by_coin_flip());
    const auto winner_heads = heads.coin_flip_winner();
    CHECK((winner_heads == 0 || winner_heads == 3));  // A or E took the spot
    CHECK(heads.top2()[1] == winner_heads);

    ClubRound tails(1);  // opposite parity flips the other way
    tails.record_set_result(TeamId::A, 3, 2);
    tails.record_set_result(TeamId::B, 3, 2);
    REQUIRE(tails.decided_by_coin_flip());
    CHECK(tails.coin_flip_winner() != winner_heads);

    // Both outcomes keep the 2-win player on top and the 0-win player last.
    for (const ClubRound* round : {&heads, &tails}) {
        CHECK(round->standings()[0].wins == 2);
        CHECK(round->standings()[3].wins == 0);
    }
}

TEST_CASE("results after completion are stable and extra records are ignored") {
    ClubRound round;
    round.record_set_result(TeamId::A, 3, 1);
    round.record_set_result(TeamId::B, 3, 0);
    const auto before = round.standings();
    round.record_set_result(TeamId::A, 3, 0);  // ignored
    const auto after = round.standings();
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(before[i].slot == after[i].slot);
        CHECK(before[i].wins == after[i].wins);
        CHECK(before[i].differential == after[i].differential);
    }
}
