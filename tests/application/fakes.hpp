#pragma once

#include <functional>
#include <vector>

#include "padel/application/clock.hpp"
#include "padel/application/event_store.hpp"

namespace padel::application::testing {

class FakeClock : public IClock {
public:
    std::uint64_t now_ms() const override { return now_; }
    void advance(std::uint64_t ms) { now_ += ms; }

private:
    std::uint64_t now_ = 1000;
};

class FakeEventStore : public IEventStore {
public:
    bool append(const CommittedEvent& event) override {
        ++append_calls;
        if (probe) {
            probe(event);
        }
        if (fail) {
            return false;
        }
        events.push_back(event);
        return true;
    }

    std::size_t point_count() const {
        std::size_t count = 0;
        for (const CommittedEvent& event : events) {
            if (std::holds_alternative<domain::PointAwarded>(event.payload)) {
                ++count;
            }
        }
        return count;
    }

    std::vector<CommittedEvent> events{};
    bool fail = false;
    int append_calls = 0;
    // Invoked before the append result is decided; used to assert ordering
    // (durability precedes apply/ACK).
    std::function<void(const CommittedEvent&)> probe{};
};

}  // namespace padel::application::testing
