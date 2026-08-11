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
