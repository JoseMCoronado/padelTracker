#include <catch2/catch_test_macros.hpp>

#include <string>

#include "padel/common/idle_dim.hpp"

using namespace padel::power;

namespace {

constexpr std::uint32_t kMin = 60u * 1000u;

}  // namespace

TEST_CASE("stage_for_idle walks awake -> dimmed -> off") {
    const IdlePolicy policy{};  // 10 min / 30 min / 15%
    CHECK(stage_for_idle(0, policy) == DisplayStage::Awake);
    CHECK(stage_for_idle(10 * kMin - 1, policy) == DisplayStage::Awake);
    CHECK(stage_for_idle(10 * kMin, policy) == DisplayStage::Dimmed);
    CHECK(stage_for_idle(30 * kMin - 1, policy) == DisplayStage::Dimmed);
    CHECK(stage_for_idle(30 * kMin, policy) == DisplayStage::Off);
    CHECK(stage_for_idle(24u * 60u * kMin, policy) == DisplayStage::Off);
}

TEST_CASE("stage_for_idle honours the disable knobs") {
    IdlePolicy no_dim{};
    no_dim.dim_percent = 0;
    CHECK(stage_for_idle(24u * 60u * kMin, no_dim) == DisplayStage::Awake);

    IdlePolicy no_window{};
    no_window.dim_after_ms = 0;
    CHECK(stage_for_idle(24u * 60u * kMin, no_window) == DisplayStage::Awake);

    IdlePolicy dim_only{};
    dim_only.off_after_ms = 0;
    CHECK(stage_for_idle(24u * 60u * kMin, dim_only) == DisplayStage::Dimmed);
}

TEST_CASE("an off window before the dim window still dims first") {
    IdlePolicy policy{};
    policy.dim_after_ms = 10 * kMin;
    policy.off_after_ms = 5 * kMin;
    CHECK(stage_for_idle(7 * kMin, policy) == DisplayStage::Awake);
    CHECK(stage_for_idle(10 * kMin, policy) == DisplayStage::Off);
}

TEST_CASE("applied_percent keeps the organizer level while awake") {
    const IdlePolicy policy{};
    CHECK(applied_percent(DisplayStage::Awake, 100, policy) == 100);
    CHECK(applied_percent(DisplayStage::Awake, 40, policy) == 40);
}

TEST_CASE("dimming never brightens a level already below the dim target") {
    const IdlePolicy policy{};  // dim_percent = 15
    CHECK(applied_percent(DisplayStage::Dimmed, 100, policy) == 15);
    CHECK(applied_percent(DisplayStage::Dimmed, 15, policy) == 15);
    CHECK(applied_percent(DisplayStage::Dimmed, 10, policy) == 10);
}

TEST_CASE("the off stage always writes zero") {
    const IdlePolicy policy{};
    CHECK(applied_percent(DisplayStage::Off, 100, policy) == 0);
    CHECK(applied_percent(DisplayStage::Off, 10, policy) == 0);
}

TEST_CASE("stage labels are diagnostics friendly") {
    CHECK(std::string(stage_label(DisplayStage::Awake)) == "awake");
    CHECK(std::string(stage_label(DisplayStage::Dimmed)) == "dimmed");
    CHECK(std::string(stage_label(DisplayStage::Off)) == "backlight off");
}
