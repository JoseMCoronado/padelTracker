cmake_minimum_required(VERSION 3.20)

# Native (host) build of the hardware-independent core: domain, protocol,
# tests, and the CLI simulator. Firmware builds (ESP-IDF) will consume the
# same components/ directories via their own project files in M2+.
project(padel_smart_court LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PADEL_BUILD_TESTS "Build native unit tests" ON)

add_compile_options(-Wall -Wextra)

add_subdirectory(components/common)
add_subdirectory(components/domain)
add_subdirectory(components/protocol)
add_subdirectory(simulator/scorer-cli)

if(PADEL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
