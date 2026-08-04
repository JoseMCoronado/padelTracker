#pragma once

#include <cstdint>

namespace padel::application {

// Injected monotonic clock (spec section 23.1: inject clock for tests).
// Milliseconds since an arbitrary epoch; must never go backwards.
class IClock {
public:
    virtual ~IClock() = default;
    virtual std::uint64_t now_ms() const = 0;
};

}  // namespace padel::application
