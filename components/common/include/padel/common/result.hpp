#pragma once

#include <cassert>
#include <utility>
#include <variant>

namespace padel {

// Minimal typed result for firmware-friendly error handling (no exceptions).
// T and E must be distinct types.
template <typename T, typename E>
class Result {
public:
    static Result ok(T value) { return Result(std::in_place_index<0>, std::move(value)); }
    static Result err(E error) { return Result(std::in_place_index<1>, std::move(error)); }

    bool has_value() const { return storage_.index() == 0; }
    explicit operator bool() const { return has_value(); }

    const T& value() const {
        assert(has_value());
        return std::get<0>(storage_);
    }
    T& value() {
        assert(has_value());
        return std::get<0>(storage_);
    }

    const E& error() const {
        assert(!has_value());
        return std::get<1>(storage_);
    }

private:
    template <std::size_t I, typename V>
    Result(std::in_place_index_t<I> tag, V&& v) : storage_(tag, std::forward<V>(v)) {}

    std::variant<T, E> storage_;
};

}  // namespace padel
