#include <catch2/catch_test_macros.hpp>

#include "padel/protocol/dedup.hpp"

using namespace padel::protocol;

namespace {

IntentIdentity intent(std::uint32_t remote, std::uint32_t boot, std::uint32_t seq) {
    return IntentIdentity{remote, boot, seq};
}

}  // namespace

TEST_CASE("dedup: first intent from a remote is new", "[dedup]") {
    Deduplicator dedup;
    REQUIRE(dedup.check_and_record(intent(1001, 42, 1)) == DedupResult::New);
}

TEST_CASE("dedup: identical retry is a duplicate", "[dedup]") {
    Deduplicator dedup;
    dedup.check_and_record(intent(1001, 42, 1));

    // Retries reuse the same identity (spec section 10.5).
    for (int retry = 0; retry < 5; ++retry) {
        REQUIRE(dedup.check_and_record(intent(1001, 42, 1)) == DedupResult::Duplicate);
    }
    REQUIRE(dedup.counters().accepted == 1);
    REQUIRE(dedup.counters().duplicates == 5);
}

TEST_CASE("dedup: sequences advance per remote independently", "[dedup]") {
    Deduplicator dedup;
    REQUIRE(dedup.check_and_record(intent(1001, 42, 1)) == DedupResult::New);
    REQUIRE(dedup.check_and_record(intent(2002, 77, 1)) == DedupResult::New);
    REQUIRE(dedup.check_and_record(intent(1001, 42, 2)) == DedupResult::New);
    REQUIRE(dedup.check_and_record(intent(2002, 77, 1)) == DedupResult::Duplicate);
    REQUIRE(dedup.check_and_record(intent(2002, 77, 2)) == DedupResult::New);
}

TEST_CASE("dedup: recent past within window is duplicate, beyond is stale", "[dedup]") {
    Deduplicator dedup(/*duplicate_window=*/64);
    dedup.check_and_record(intent(1001, 42, 1000));

    REQUIRE(dedup.check_and_record(intent(1001, 42, 999)) == DedupResult::Duplicate);
    REQUIRE(dedup.check_and_record(intent(1001, 42, 1000 - 64)) == DedupResult::Duplicate);
    REQUIRE(dedup.check_and_record(intent(1001, 42, 1000 - 65)) == DedupResult::Stale);
    REQUIRE(dedup.counters().stale == 1);
}

TEST_CASE("dedup: new boot_id resets the sequence space", "[dedup]") {
    Deduplicator dedup;
    dedup.check_and_record(intent(1001, 42, 500));

    // Remote rebooted: random new boot_id, sequence restarts low.
    REQUIRE(dedup.check_and_record(intent(1001, 43, 1)) == DedupResult::New);
    REQUIRE(dedup.check_and_record(intent(1001, 43, 1)) == DedupResult::Duplicate);
    // Old boot's identity no longer matches the stored boot_id and is
    // treated as a fresh space, not replayed silently as new points: the
    // watermark now tracks boot 43.
    REQUIRE(dedup.check_and_record(intent(1001, 43, 2)) == DedupResult::New);
}

TEST_CASE("dedup: sequence wrap is handled with serial arithmetic", "[dedup]") {
    Deduplicator dedup;
    dedup.check_and_record(intent(1001, 42, 0xFFFFFFFF));

    // Wrap: 0 is "ahead" of 0xFFFFFFFF.
    REQUIRE(dedup.check_and_record(intent(1001, 42, 0)) == DedupResult::New);
    REQUIRE(dedup.check_and_record(intent(1001, 42, 0xFFFFFFFF)) == DedupResult::Duplicate);
    REQUIRE(dedup.check_and_record(intent(1001, 42, 1)) == DedupResult::New);
}

TEST_CASE("dedup: two-phase classify then record", "[dedup]") {
    Deduplicator dedup;
    const IntentIdentity id = intent(1001, 42, 7);

    REQUIRE(dedup.classify(id) == DedupResult::New);
    // Not recorded yet: still classified as new (e.g. storage commit failed,
    // point was not applied, a retry must be processed as new).
    REQUIRE(dedup.classify(id) == DedupResult::New);

    dedup.record(id);
    REQUIRE(dedup.classify(id) == DedupResult::Duplicate);
}

TEST_CASE("dedup: snapshot and restore preserve classification", "[dedup][persistence]") {
    Deduplicator dedup;
    dedup.check_and_record(intent(1001, 42, 88));
    dedup.check_and_record(intent(2002, 77, 3));

    // Court reboots; dedup state is restored from persistence (M3). The
    // remote retries its unacknowledged intent — it must be Duplicate, never
    // applied twice (spec section 13.5).
    Deduplicator recovered;
    recovered.restore(dedup.snapshot());
    REQUIRE(recovered.classify(intent(1001, 42, 88)) == DedupResult::Duplicate);
    REQUIRE(recovered.classify(intent(2002, 77, 3)) == DedupResult::Duplicate);
    REQUIRE(recovered.check_and_record(intent(1001, 42, 89)) == DedupResult::New);
}

TEST_CASE("dedup: capacity is bounded", "[dedup]") {
    Deduplicator dedup;
    for (std::uint32_t remote = 1; remote <= Deduplicator::kMaxRemotes; ++remote) {
        REQUIRE(dedup.check_and_record(intent(remote, 1, 1)) == DedupResult::New);
    }
    // Ninth remote: no capacity; rejected rather than silently evicting a
    // known remote's dedup state.
    REQUIRE(dedup.check_and_record(intent(999, 1, 1)) == DedupResult::Stale);
}
