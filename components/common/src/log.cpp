#include "padel/common/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace padel::logging {
namespace {

struct State {
    std::mutex mutex;
    Entry ring[kRingCapacity];
    std::size_t next = 0;
    std::size_t count = 0;
    std::uint32_t emitted = 0;
    std::uint64_t (*clock)() = nullptr;
    void (*sink)(const Entry&) = nullptr;
};

State& state() {
    static State instance;
    return instance;
}

void copy_bounded(char* dst, std::size_t dst_size, const char* src) {
    std::snprintf(dst, dst_size, "%s", src);
}

}  // namespace

void set_clock(std::uint64_t (*now_ms)()) {
    std::lock_guard<std::mutex> lock(state().mutex);
    state().clock = now_ms;
}

void set_sink(void (*sink)(const Entry&)) {
    std::lock_guard<std::mutex> lock(state().mutex);
    state().sink = sink;
}

void emit(Level level, const char* event, const char* detail_fmt, ...) {
    Entry entry{};
    entry.level = level;
    copy_bounded(entry.event, sizeof(entry.event), event);

    va_list args;
    va_start(args, detail_fmt);
    std::vsnprintf(entry.detail, sizeof(entry.detail), detail_fmt, args);
    va_end(args);

    void (*sink)(const Entry&) = nullptr;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        if (state().clock != nullptr) {
            entry.t_ms = state().clock();
        }
        state().ring[state().next] = entry;
        state().next = (state().next + 1) % kRingCapacity;
        if (state().count < kRingCapacity) {
            ++state().count;
        }
        ++state().emitted;
        sink = state().sink;
    }
    if (sink != nullptr) {
        sink(entry);
    }
}

std::vector<Entry> recent_entries(std::size_t max_entries) {
    std::lock_guard<std::mutex> lock(state().mutex);
    const std::size_t available = state().count;
    const std::size_t take = max_entries < available ? max_entries : available;
    std::vector<Entry> entries;
    entries.reserve(take);
    // Oldest of the selected window first, newest last.
    for (std::size_t i = available - take; i < available; ++i) {
        const std::size_t index =
            (state().next + kRingCapacity - available + i) % kRingCapacity;
        entries.push_back(state().ring[index]);
    }
    return entries;
}

std::vector<std::string> recent_lines(std::size_t max_lines) {
    std::vector<std::string> lines;
    for (const Entry& entry : recent_entries(max_lines)) {
        std::string line = entry.event;
        if (entry.detail[0] != '\0') {
            line += ' ';
            line += entry.detail;
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::uint32_t total_emitted() {
    std::lock_guard<std::mutex> lock(state().mutex);
    return state().emitted;
}

void reset() {
    std::lock_guard<std::mutex> lock(state().mutex);
    state().next = 0;
    state().count = 0;
    state().emitted = 0;
    state().clock = nullptr;
    state().sink = nullptr;
}

}  // namespace padel::logging
