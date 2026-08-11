#include "padel/common/idle_dim.hpp"

#include <algorithm>

namespace padel::power {

DisplayStage stage_for_idle(std::uint32_t idle_ms, const IdlePolicy& policy) {
    if (policy.dim_percent == 0 || policy.dim_after_ms == 0) {
        return DisplayStage::Awake;
    }
    if (idle_ms < policy.dim_after_ms) {
        return DisplayStage::Awake;
    }
    // A misconfigured off window that lands before the dim one still has to
    // pass through Dimmed first.
    if (policy.off_after_ms != 0 &&
        idle_ms >= std::max(policy.off_after_ms, policy.dim_after_ms)) {
        return DisplayStage::Off;
    }
    return DisplayStage::Dimmed;
}

std::uint8_t applied_percent(DisplayStage stage, std::uint8_t user_percent,
                             const IdlePolicy& policy) {
    switch (stage) {
        case DisplayStage::Awake:
            return user_percent;
        case DisplayStage::Dimmed:
            return std::min(user_percent, policy.dim_percent);
        case DisplayStage::Off:
            break;
    }
    return 0;
}

const char* stage_label(DisplayStage stage) {
    switch (stage) {
        case DisplayStage::Awake:
            return "awake";
        case DisplayStage::Dimmed:
            return "dimmed";
        case DisplayStage::Off:
            break;
    }
    return "backlight off";
}

}  // namespace padel::power
