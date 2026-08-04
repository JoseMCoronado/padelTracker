// Spec section 18.4: journal format and recovery tests.
#include <catch2/catch_test_macros.hpp>

#include "padel/domain/match_engine.hpp"
#include "padel/persistence/file_backend.hpp"
#include "padel/persistence/journal.hpp"

using namespace padel;
using namespace padel::application;
using namespace padel::persistence;

namespace {

CommittedEvent make_event(EventId id, domain::Event payload,
                          std::optional<protocol::IntentIdentity> intent = std::nullopt) {
    CommittedEvent event{};
    event.event_id = id;
    event.match_id = 42;
    event.state_revision = id;
    event.payload = std::move(payload);
    event.source = intent ? InputSource::Remote : InputSource::TouchscreenAdmin;
    event.intent = intent;
    event.monotonic_ms = 1000 + id;
    return event;
}

std::vector<CommittedEvent> sample_events() {
    std::vector<CommittedEvent> events;
    events.push_back(make_event(1, domain::MatchCreated{42, domain::preset_club_mini_set()}));
    events.push_back(make_event(2, domain::MatchStarted{TeamId::B}));
    events.push_back(make_event(3, domain::PointAwarded{TeamId::A, InputSource::Remote},
                                protocol::IntentIdentity{0xA0, 7, 1}));
    events.push_back(make_event(4, domain::ServingTeamChanged{TeamId::A}));
    events.push_back(make_event(5, domain::MatchPaused{}));
    events.push_back(make_event(6, domain::MatchResumed{}));
    events.push_back(make_event(7, domain::PointAwarded{TeamId::B, InputSource::PhysicalBackupButton}));
    events.push_back(make_event(8, domain::ScoringActionUndone{7}));
    events.push_back(make_event(9, domain::MatchFinishedManually{TeamId::A}));
    events.push_back(make_event(10, domain::MatchReset{}));
    return events;
}

std::vector<std::uint8_t> journal_bytes(const std::vector<CommittedEvent>& events) {
    std::vector<std::uint8_t> bytes;
    for (const CommittedEvent& event : events) {
        const auto record = serialize_record(event);
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    return bytes;
}

}  // namespace

TEST_CASE("every event type round-trips through a journal record") {
    const auto events = sample_events();
    const auto result = recover(journal_bytes(events));

    CHECK(result.tail == TailStatus::Clean);
    REQUIRE(result.events.size() == events.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        const CommittedEvent& in = events[i];
        const CommittedEvent& out = result.events[i];
        CHECK(out.event_id == in.event_id);
        CHECK(out.match_id == in.match_id);
        CHECK(out.state_revision == in.state_revision);
        CHECK(out.monotonic_ms == in.monotonic_ms);
        CHECK(out.source == in.source);
        CHECK(out.intent == in.intent);
        CHECK(out.payload.index() == in.payload.index());
    }

    // Spot-check payload fields survived.
    const auto& created = std::get<domain::MatchCreated>(result.events[0].payload);
    CHECK(created.match_id == 42);
    CHECK(created.config.normal_set.games_to_win == 3);
    CHECK(created.config.game_rule == domain::GameRule::GoldenPoint);
    const auto& point = std::get<domain::PointAwarded>(result.events[2].payload);
    CHECK(point.team == TeamId::A);
    REQUIRE(result.events[2].intent.has_value());
    CHECK(result.events[2].intent->sequence == 1);
    const auto& undone = std::get<domain::ScoringActionUndone>(result.events[7].payload);
    CHECK(undone.undone_event_id == 7);
    const auto& finished = std::get<domain::MatchFinishedManually>(result.events[8].payload);
    REQUIRE(finished.declared_winner.has_value());
    CHECK(*finished.declared_winner == TeamId::A);
}

TEST_CASE("empty journal recovers to a clean empty result") {
    const auto result = recover({});
    CHECK(result.tail == TailStatus::Clean);
    CHECK(result.events.empty());
    CHECK(result.valid_bytes == 0);
}

TEST_CASE("replayed journal reproduces engine state including undo") {
    // Build a real history through the engine, journal it, replay it.
    domain::MatchEngine engine(domain::preset_standard_advantage());
    std::vector<CommittedEvent> journaled;
    // Genesis event, journaled the way CourtService does at construction.
    CommittedEvent genesis{};
    genesis.event_id = engine.journal().front().id;
    genesis.state_revision = 1;
    genesis.payload = engine.journal().front().payload;
    journaled.push_back(genesis);
    const auto run = [&](auto cmd) {
        const auto decided = engine.decide(cmd);
        REQUIRE(decided.has_value());
        CommittedEvent event{};
        event.event_id = decided.value().id;
        event.state_revision = decided.value().revision_after;
        event.payload = decided.value().payload;
        journaled.push_back(event);
        engine.commit(decided.value());
    };

    run(domain::StartMatch{TeamId::A});
    run(domain::AwardPoint{TeamId::A, InputSource::Simulator});
    run(domain::AwardPoint{TeamId::A, InputSource::Simulator});
    run(domain::AwardPoint{TeamId::B, InputSource::Simulator});
    run(domain::UndoLastScoringAction{});

    const auto recovered = recover(journal_bytes(journaled));
    REQUIRE(recovered.tail == TailStatus::Clean);
    std::vector<domain::StoredEvent> stored;
    for (const CommittedEvent& event : recovered.events) {
        stored.push_back(domain::StoredEvent{event.event_id, event.payload});
    }
    const auto replayed =
        domain::MatchEngine::replay(std::move(stored), domain::preset_standard_advantage());

    CHECK(replayed.state().current_game.raw_points_a == engine.state().current_game.raw_points_a);
    CHECK(replayed.state().current_game.raw_points_b == engine.state().current_game.raw_points_b);
    CHECK(replayed.state().current_game.raw_points_b == 0);  // undo took effect
    CHECK(replayed.state().revision == engine.state().revision);
    CHECK(replayed.state().lifecycle == engine.state().lifecycle);
}

TEST_CASE("corrupt final record: replay stops at last valid record") {
    const auto events = sample_events();
    auto bytes = journal_bytes(events);

    SECTION("flipped byte inside the last record") {
        bytes[bytes.size() - 5] ^= 0xFF;
        const auto result = recover(bytes);
        CHECK(result.tail == TailStatus::CorruptRecord);
        CHECK(result.events.size() == events.size() - 1);
    }

    SECTION("bad magic at a record boundary") {
        const auto prefix = journal_bytes({events[0]});
        bytes[prefix.size()] = 0x00;
        const auto result = recover(bytes);
        CHECK(result.tail == TailStatus::CorruptRecord);
        CHECK(result.events.size() == 1);
        CHECK(result.valid_bytes == prefix.size());
    }

    SECTION("insane record length") {
        const auto prefix = journal_bytes({events[0]});
        bytes[prefix.size() + 3] = 0xFF;  // length low byte
        bytes[prefix.size() + 4] = 0xFF;  // length high byte
        const auto result = recover(bytes);
        CHECK(result.tail == TailStatus::CorruptRecord);
        CHECK(result.events.size() == 1);
    }
}

TEST_CASE("truncated final record: replay stops and reports the torn tail") {
    const auto events = sample_events();
    const auto full = journal_bytes(events);

    for (const std::size_t cut : {std::size_t{1}, std::size_t{3}, std::size_t{20}}) {
        std::vector<std::uint8_t> bytes(full.begin(), full.end() - cut);
        const auto result = recover(bytes);
        CHECK(result.tail == TailStatus::TruncatedRecord);
        CHECK(result.events.size() == events.size() - 1);
        CHECK(result.valid_bytes <= bytes.size());
    }
}

TEST_CASE("newer schema version stops replay without misreading") {
    const auto events = sample_events();
    auto bytes = journal_bytes({events[0]});
    const auto prefix_size = bytes.size();
    auto second = serialize_record(events[1]);
    second[2] = kJournalSchemaVersion + 1;  // pretend a future firmware wrote it
    bytes.insert(bytes.end(), second.begin(), second.end());

    const auto result = recover(bytes);
    CHECK(result.tail == TailStatus::UnsupportedSchema);
    CHECK(result.events.size() == 1);
    CHECK(result.valid_bytes == prefix_size);
}

TEST_CASE("JournalWriter appends durably and rolls back failed appends") {
    InMemoryFileBackend backend;
    JournalWriter writer(backend, 0);
    const auto events = sample_events();

    REQUIRE(writer.append(events[0]));
    const std::size_t committed = writer.committed_size();
    CHECK(backend.durable_size() == committed);

    SECTION("torn append is rolled back; journal stays clean") {
        backend.tear_next_append(10);
        CHECK_FALSE(writer.append(events[1]));
        CHECK(backend.size() == committed);  // partial bytes truncated away

        REQUIRE(writer.append(events[1]));
        const auto result = recover(backend.read_all());
        CHECK(result.tail == TailStatus::Clean);
        CHECK(result.events.size() == 2);
    }

    SECTION("failed sync is rolled back") {
        backend.fail_syncs(true);
        CHECK_FALSE(writer.append(events[1]));
        backend.fail_syncs(false);
        REQUIRE(writer.append(events[1]));
        const auto result = recover(backend.read_all());
        CHECK(result.tail == TailStatus::Clean);
        CHECK(result.events.size() == 2);
    }
}
