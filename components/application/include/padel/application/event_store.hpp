#pragma once

#include <cstdint>
#include <optional>

#include "padel/common/ids.hpp"
#include "padel/domain/events.hpp"
#include "padel/protocol/packets.hpp"

namespace padel::application {

// One journaled fact: the domain event plus the metadata persistence and
// recovery need (spec section 13.2). The intent identity is carried for
// remote-sourced points so the deduplication watermarks can be rebuilt by
// replay after a reboot (ADR-0007).
struct CommittedEvent {
    EventId event_id{};
    MatchId match_id{};
    std::uint64_t state_revision{};
    domain::Event payload{};
    InputSource source{InputSource::Simulator};
    std::optional<protocol::IntentIdentity> intent{};
    std::uint64_t monotonic_ms{};
};

// Durable event sink. append() returning true means the record survives
// power loss (write + sync, spec section 13.3); the service only ACKs
// Accepted after that guarantee.
class IEventStore {
public:
    virtual ~IEventStore() = default;
    virtual bool append(const CommittedEvent& event) = 0;
};

}  // namespace padel::application
