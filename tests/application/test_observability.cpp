// Verifies the application layer emits the stable structured events
// (spec section 16) as side effects of the normal command flow.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "padel/application/court_service.hpp"
#include "padel/common/log.hpp"
#include "padel/domain/types.hpp"

using namespace padel;

namespace {

class FakeClock : public application::IClock {
public:
    std::uint64_t now_ms() const override { return now; }
    std::uint64_t now = 1000;
};

class MemoryStore : public application::IEventStore {
public:
    bool append(const application::CommittedEvent& event) override {
        if (fail) {
            return false;
        }
        events.push_back(event);
        return true;
    }
    std::vector<application::CommittedEvent> events;
    bool fail = false;
};

struct RingReset {
    RingReset() { logging::reset(); }
    ~RingReset() { logging::reset(); }
};

bool has_event(const std::string& name) {
    const auto lines = logging::recent_lines(logging::kRingCapacity);
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) {
        return line.rfind(name, 0) == 0;
    });
}

}  // namespace

TEST_CASE("point flow emits match.started and match.point_accepted") {
    RingReset guard;
    FakeClock clock;
    MemoryStore store;
    application::CourtService service({1, 0}, domain::preset_standard_advantage(), store, clock);

    REQUIRE(service.start_match(TeamId::A));
    service.award_point_local(TeamId::A, InputSource::TouchscreenAdmin);

    CHECK(has_event("match.started serving=A"));
    CHECK(has_event("match.point_accepted team=A"));
}

TEST_CASE("storage failure emits storage.commit_failed") {
    RingReset guard;
    FakeClock clock;
    MemoryStore store;
    application::CourtService service({1, 0}, domain::preset_standard_advantage(), store, clock);
    REQUIRE(service.start_match(TeamId::A));

    store.fail = true;
    service.award_point_local(TeamId::A, InputSource::TouchscreenAdmin);

    CHECK(has_event("storage.commit_failed"));
}

TEST_CASE("opposing wired presses emit conflict open and resolve events") {
    RingReset guard;
    FakeClock clock;
    MemoryStore store;
    application::CourtService service({1, 250}, domain::preset_standard_advantage(), store, clock);
    REQUIRE(service.start_match(TeamId::A));

    service.award_point_local(TeamId::A, InputSource::PhysicalBackupButton);
    service.award_point_local(TeamId::B, InputSource::PhysicalBackupButton);
    CHECK(has_event("match.conflict_opened"));

    REQUIRE(service.resolve_conflict(TeamId::B));
    CHECK(has_event("match.conflict_resolved outcome=team_B"));
}
