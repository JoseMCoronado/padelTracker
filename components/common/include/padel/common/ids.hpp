#pragma once

#include <cstdint>

namespace padel {

using CourtId = std::uint16_t;
using MatchId = std::uint64_t;
using EventId = std::uint64_t;
using RemoteId = std::uint32_t;
using PlayerId = std::uint64_t;  // future coordinator

enum class TeamId : std::uint8_t {
    A = 1,
    B = 2,
};

constexpr TeamId opponent(TeamId team) {
    return team == TeamId::A ? TeamId::B : TeamId::A;
}

// Every input path (remote, wired button, touch, simulator) converges on the
// same command path; the source is carried for diagnostics and policy.
enum class InputSource : std::uint8_t {
    Remote = 1,
    PhysicalBackupButton = 2,
    TouchscreenAdmin = 3,
    CoordinatorOverride = 4,  // future
    Simulator = 5,
};

}  // namespace padel
