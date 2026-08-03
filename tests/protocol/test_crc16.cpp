#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "padel/protocol/crc16.hpp"

using padel::protocol::crc16_ccitt;

TEST_CASE("crc16: CCITT-FALSE check value", "[crc16]") {
    const char* input = "123456789";
    REQUIRE(crc16_ccitt(reinterpret_cast<const std::uint8_t*>(input), std::strlen(input)) ==
            0x29B1);
}

TEST_CASE("crc16: empty input yields initial value", "[crc16]") {
    REQUIRE(crc16_ccitt(nullptr, 0) == 0xFFFF);
}

TEST_CASE("crc16: single bit flip changes the checksum", "[crc16]") {
    std::uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::uint16_t original = crc16_ccitt(data, sizeof(data));
    data[3] ^= 0x01;
    REQUIRE(crc16_ccitt(data, sizeof(data)) != original);
}
