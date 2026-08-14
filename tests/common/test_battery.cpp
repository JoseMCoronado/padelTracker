#include <catch2/catch_test_macros.hpp>

#include "padel/common/battery.hpp"

using namespace padel::battery;

TEST_CASE("mv_to_percent is unknown below the no-cell threshold") {
    CHECK_FALSE(mv_to_percent(0).has_value());
    CHECK_FALSE(mv_to_percent(2499).has_value());
}

TEST_CASE("mv_to_percent hits the curve anchors") {
    CHECK(mv_to_percent(4200) == 100);
    CHECK(mv_to_percent(4500) == 100);
    CHECK(mv_to_percent(3900) == 75);
    CHECK(mv_to_percent(3700) == 50);
    CHECK(mv_to_percent(3500) == 25);
    CHECK(mv_to_percent(3300) == 10);
    CHECK(mv_to_percent(3000) == 0);
    CHECK(mv_to_percent(2800) == 0);
}

TEST_CASE("mv_to_percent interpolates between anchors") {
    // Halfway 3.70 V (50%) → 3.90 V (75%) ≈ 3.80 V → 62–63%.
    const auto mid = mv_to_percent(3800);
    REQUIRE(mid.has_value());
    CHECK(*mid >= 60);
    CHECK(*mid <= 65);
}

TEST_CASE("estimate_runtime_min scales with SoC and capacity") {
    CHECK(estimate_runtime_min(0) == 0);
    CHECK(estimate_runtime_min(100, 2000, 500) == 240);  // 2000/500 * 60
    CHECK(estimate_runtime_min(50, 2000, 500) == 120);
    CHECK(estimate_runtime_min(100, 2000, 0) == 0);
}

TEST_CASE("format_runtime_estimate labels") {
    CHECK(format_runtime_estimate(std::nullopt) == "unknown");
    CHECK(format_runtime_estimate(0) == "~0m");
    CHECK(format_runtime_estimate(100, 2000, 500) == "~4h 0m");
    CHECK(format_runtime_estimate(10, 2000, 500) == "~24m");
}

TEST_CASE("median_mv picks the middle sample") {
    const std::uint16_t odd[] = {3700, 3950, 3720, 3710, 3730};
    CHECK(median_mv(odd, 5) == 3720);

    // One wildly corrupt read in the burst cannot drag the result.
    const std::uint16_t corrupt[] = {3700, 3705, 9900, 3710, 3702};
    CHECK(median_mv(corrupt, 5) == 3705);

    // Even count averages the two middles, rounded up.
    const std::uint16_t even[] = {3700, 3701, 3702, 3705};
    CHECK(median_mv(even, 4) == 3702);

    CHECK_FALSE(median_mv(odd, 0).has_value());
    CHECK_FALSE(median_mv(nullptr, 3).has_value());
}

TEST_CASE("BatteryMonitor seeds on the first good sample") {
    BatteryMonitor monitor;
    CHECK_FALSE(monitor.percent().has_value());

    monitor.add_sample(3900);
    // Seeded outright, not ramped up from zero one point at a time.
    CHECK(monitor.percent() == 75);
    CHECK(monitor.smoothed_mv() == 3900);
    CHECK(monitor.misses() == 0);
}

TEST_CASE("BatteryMonitor absorbs the jump that made the display wonky") {
    BatteryMonitor monitor;
    monitor.add_sample(3948);  // ~79%
    const auto seeded = monitor.percent();
    REQUIRE(seeded.has_value());

    // The reported symptom: one sample lands ~250 mV low, which the raw
    // mapping showed as a drop straight from 79% to 50%.
    CHECK(mv_to_percent(3700) == 50);
    monitor.add_sample(3700);

    // A sag that size is plausible enough to be a real load transient, so it
    // is smoothed rather than thrown away: at most one point of movement.
    CHECK(monitor.misses() == 0);
    REQUIRE(monitor.percent().has_value());
    CHECK(*monitor.percent() >= *seeded - 1);
    CHECK(*monitor.percent() <= *seeded);
}

TEST_CASE("BatteryMonitor rejects a jump no cell could make") {
    BatteryMonitor monitor;
    monitor.add_sample(3900);
    REQUIRE(monitor.percent() == 75);

    // Corrupt I2C reads land far outside anything a cell does between
    // samples, so these are dropped rather than averaged in.
    monitor.add_sample(3300);
    CHECK(monitor.percent() == 75);
    CHECK(monitor.smoothed_mv() == 3900);
    CHECK(monitor.misses() == 1);

    // A good read clears the streak.
    monitor.add_sample(3895);
    CHECK(monitor.misses() == 0);
}

TEST_CASE("BatteryMonitor cannot get stuck behind its own outlier gate") {
    BatteryMonitor::Config config;
    config.misses_to_unknown = 3;
    BatteryMonitor monitor{config};
    monitor.add_sample(3900);
    REQUIRE(monitor.percent() == 75);

    // A cell collapsing under load can genuinely move further than the gate
    // allows. Rejecting forever would pin the display at a stale 75%, so the
    // streak gives up and re-seeds on the new reality.
    for (int i = 0; i < 3; ++i) {
        monitor.add_sample(3300);
    }
    CHECK_FALSE(monitor.percent().has_value());

    monitor.add_sample(3300);
    CHECK(monitor.percent() == 10);
    CHECK(monitor.low());
}

TEST_CASE("BatteryMonitor still follows a real decline") {
    BatteryMonitor monitor;
    monitor.add_sample(3900);
    REQUIRE(monitor.percent() == 75);

    // A genuine drain arrives as many small steps, not one cliff.
    for (int mv = 3890; mv >= 3700; mv -= 10) {
        for (int repeat = 0; repeat < 8; ++repeat) {
            monitor.add_sample(static_cast<std::uint16_t>(mv));
        }
    }
    REQUIRE(monitor.percent().has_value());
    CHECK(*monitor.percent() < 75);
    CHECK(*monitor.percent() <= 55);
}

TEST_CASE("BatteryMonitor rises again while charging") {
    BatteryMonitor monitor;
    monitor.add_sample(3700);
    REQUIRE(monitor.percent() == 50);

    for (int mv = 3710; mv <= 3900; mv += 10) {
        for (int repeat = 0; repeat < 8; ++repeat) {
            monitor.add_sample(static_cast<std::uint16_t>(mv));
        }
    }
    REQUIRE(monitor.percent().has_value());
    CHECK(*monitor.percent() > 50);
}

TEST_CASE("BatteryMonitor never moves more than one point per sample") {
    BatteryMonitor monitor;
    monitor.add_sample(3700);
    auto previous = monitor.percent();
    REQUIRE(previous.has_value());

    // Inside the outlier gate, so every sample is accepted and the only thing
    // holding the display back is the slew limit.
    for (int i = 0; i < 40; ++i) {
        monitor.add_sample(3990);
        REQUIRE(monitor.percent().has_value());
        const int step = static_cast<int>(*monitor.percent()) - static_cast<int>(*previous);
        CHECK(step >= -1);
        CHECK(step <= 1);
        previous = monitor.percent();
    }
}

TEST_CASE("BatteryMonitor holds the last value across failed reads") {
    BatteryMonitor::Config config;
    config.misses_to_unknown = 3;
    BatteryMonitor monitor{config};
    monitor.add_sample(3900);
    REQUIRE(monitor.percent() == 75);

    monitor.add_sample(std::nullopt);
    CHECK(monitor.percent() == 75);  // no flicker to "BAT --" on one bad read
    monitor.add_sample(std::nullopt);
    CHECK(monitor.percent() == 75);

    monitor.add_sample(std::nullopt);
    CHECK_FALSE(monitor.percent().has_value());
    CHECK_FALSE(monitor.smoothed_mv().has_value());

    // A good read after going unknown re-seeds immediately.
    monitor.add_sample(3700);
    CHECK(monitor.percent() == 50);
}

TEST_CASE("BatteryMonitor treats a missing cell as unknown, not 0%") {
    BatteryMonitor::Config config;
    config.misses_to_unknown = 2;
    BatteryMonitor monitor{config};
    monitor.add_sample(0);
    CHECK_FALSE(monitor.percent().has_value());
    monitor.add_sample(1200);
    CHECK_FALSE(monitor.percent().has_value());
}

TEST_CASE("BatteryMonitor low warning has hysteresis") {
    BatteryMonitor monitor;
    monitor.add_sample(3900);
    CHECK_FALSE(monitor.low());

    // Seed straight into the warning band: 3.35 V is ~12%.
    BatteryMonitor low_monitor;
    low_monitor.add_sample(3350);
    REQUIRE(low_monitor.percent().has_value());
    REQUIRE(*low_monitor.percent() <= 15);
    CHECK(low_monitor.low());

    // Recovering to 16-19% must not clear it, or the label would flicker.
    while (low_monitor.percent() < 18) {
        low_monitor.add_sample(3430);
        REQUIRE(low_monitor.percent().has_value());
    }
    CHECK(*low_monitor.percent() >= 16);
    CHECK(low_monitor.low());

    // 20% clears it.
    while (low_monitor.percent() < 20) {
        low_monitor.add_sample(3460);
        REQUIRE(low_monitor.percent().has_value());
    }
    CHECK_FALSE(low_monitor.low());
}

TEST_CASE("BatteryMonitor reset drops back to unknown") {
    BatteryMonitor monitor;
    monitor.add_sample(3900);
    REQUIRE(monitor.percent().has_value());
    monitor.reset();
    CHECK_FALSE(monitor.percent().has_value());
    CHECK_FALSE(monitor.smoothed_mv().has_value());
    CHECK_FALSE(monitor.low());
}
