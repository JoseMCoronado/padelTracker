// Structured log ring (spec section 16): stable event names, bounded
// buffer, host clock/sink hooks.

#include <catch2/catch_test_macros.hpp>

#include "padel/common/log.hpp"

using namespace padel;

namespace {

struct RingReset {
    RingReset() { logging::reset(); }
    ~RingReset() { logging::reset(); }
};

std::uint64_t fake_now() {
    return 42'000;
}

}  // namespace

TEST_CASE("emit stores event name, detail and clock timestamp") {
    RingReset guard;
    logging::set_clock(fake_now);

    logging::emit(logging::Level::Info, "match.point_accepted", "team=%c rev=%d", 'A', 7);

    const auto entries = logging::recent_entries(10);
    REQUIRE(entries.size() == 1);
    CHECK(std::string(entries[0].event) == "match.point_accepted");
    CHECK(std::string(entries[0].detail) == "team=A rev=7");
    CHECK(entries[0].t_ms == 42'000);
    CHECK(entries[0].level == logging::Level::Info);
}

TEST_CASE("ring keeps the newest entries once capacity is exceeded") {
    RingReset guard;

    for (int i = 0; i < static_cast<int>(logging::kRingCapacity) + 10; ++i) {
        logging::emit(logging::Level::Info, "test.event", "n=%d", i);
    }

    CHECK(logging::total_emitted() == logging::kRingCapacity + 10);
    const auto entries = logging::recent_entries(logging::kRingCapacity);
    REQUIRE(entries.size() == logging::kRingCapacity);
    // Oldest surviving entry is #10; newest is the last emitted.
    CHECK(std::string(entries.front().detail) == "n=10");
    CHECK(std::string(entries.back().detail) ==
          "n=" + std::to_string(logging::kRingCapacity + 9));
}

TEST_CASE("recent_lines formats event + detail, newest last") {
    RingReset guard;
    logging::emit(logging::Level::Info, "a.first", "x=1");
    logging::emit(logging::Level::Warn, "b.second", "");

    const auto lines = logging::recent_lines(10);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "a.first x=1");
    CHECK(lines[1] == "b.second");  // empty detail leaves no trailing space
}

TEST_CASE("recent_lines caps the returned window") {
    RingReset guard;
    for (int i = 0; i < 20; ++i) {
        logging::emit(logging::Level::Info, "test.event", "n=%d", i);
    }
    const auto lines = logging::recent_lines(5);
    REQUIRE(lines.size() == 5);
    CHECK(lines.back() == "test.event n=19");
    CHECK(lines.front() == "test.event n=15");
}

TEST_CASE("oversized event and detail are truncated, not corrupted") {
    RingReset guard;
    const std::string long_name(100, 'e');
    const std::string long_detail(200, 'd');
    logging::emit(logging::Level::Error, long_name.c_str(), "%s", long_detail.c_str());

    const auto entries = logging::recent_entries(1);
    REQUIRE(entries.size() == 1);
    CHECK(std::string(entries[0].event).size() == sizeof(logging::Entry{}.event) - 1);
    CHECK(std::string(entries[0].detail).size() == sizeof(logging::Entry{}.detail) - 1);
}

TEST_CASE("sink receives every record") {
    RingReset guard;
    static int sink_calls;
    sink_calls = 0;
    logging::set_sink([](const logging::Entry&) { ++sink_calls; });

    logging::emit(logging::Level::Info, "one", "");
    logging::emit(logging::Level::Info, "two", "");
    CHECK(sink_calls == 2);
}
