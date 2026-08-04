// Structured logging with stable event names (spec section 16).
//
// Every log call carries a machine-stable event name ("match.point_accepted",
// "storage.commit_failed", ...) plus a short human detail string. Records land
// in a bounded in-memory ring surfaced on the diagnostics screen, and are
// forwarded to a host-installed sink (stdout natively, serial on device).
//
// The module is a process-wide singleton guarded by a mutex: call sites stay
// one-liners and hosts configure clock/sink once at boot.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace padel::logging {

enum class Level : std::uint8_t { Info, Warn, Error };

struct Entry {
    std::uint64_t t_ms = 0;
    Level level = Level::Info;
    char event[28] = {};   // stable name, e.g. "match.point_accepted"
    char detail[76] = {};  // formatted context, e.g. "team=A rev=214"
};

inline constexpr std::size_t kRingCapacity = 48;

// Host hooks. Both optional: without a clock, t_ms is 0; without a sink,
// records only go to the ring.
void set_clock(std::uint64_t (*now_ms)());
void set_sink(void (*sink)(const Entry&));

// Appends a record (printf-style detail). Truncates oversized fields.
void emit(Level level, const char* event, const char* detail_fmt, ...)
    __attribute__((format(printf, 3, 4)));

inline void info(const char* event, const char* fmt = "") { emit(Level::Info, event, "%s", fmt); }

// Newest-last formatted lines ("event detail") for the diagnostics screen.
std::vector<std::string> recent_lines(std::size_t max_lines);

// Newest-last raw entries (tests, serial dumps).
std::vector<Entry> recent_entries(std::size_t max_entries);

// Total records ever emitted (ring overflow diagnosis).
std::uint32_t total_emitted();

// Clears the ring and hooks (tests).
void reset();

}  // namespace padel::logging
