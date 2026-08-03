include(FetchContent)

# Pinned test framework (spec section 5.4).
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(domain_tests
    domain/test_advantage_game.cpp
    domain/test_golden_point.cpp
    domain/test_sets.cpp
    domain/test_match.cpp
    domain/test_undo.cpp
    domain/test_engine.cpp)
target_link_libraries(domain_tests PRIVATE padel_domain Catch2::Catch2WithMain)
target_include_directories(domain_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(domain_tests)

add_executable(protocol_tests
    protocol/test_crc16.cpp
    protocol/test_packets.cpp
    protocol/test_dedup.cpp)
target_link_libraries(protocol_tests PRIVATE padel_protocol Catch2::Catch2WithMain)
catch_discover_tests(protocol_tests)
