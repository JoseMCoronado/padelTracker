cmake_minimum_required(VERSION 3.20)

# Native (host) build of the hardware-independent core: domain, protocol,
# tests, and the CLI simulator. Firmware builds (ESP-IDF) will consume the
# same components/ directories via their own project files in M2+.
project(padel_smart_court LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PADEL_BUILD_TESTS "Build native unit tests" ON)
option(PADEL_BUILD_UI "Build the LVGL UI component and desktop court simulator" ON)

include(FetchContent)

if(PADEL_BUILD_UI)
    # LVGL pinned to the version the Waveshare 7B vendor demo proves (ADR-0010).
    FetchContent_Declare(
        lvgl
        GIT_REPOSITORY https://github.com/lvgl/lvgl.git
        GIT_TAG v8.4.0
        GIT_SHALLOW TRUE)
    # Configure LVGL through compile definitions instead of an lv_conf.h copy
    # (LV_CONF_SKIP enables per-option -D overrides).
    set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
    set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(lvgl)
    target_compile_definitions(lvgl PUBLIC
        LV_CONF_SKIP=1
        LV_COLOR_DEPTH=16          # matches the RGB565 panel on the device
        LV_COLOR_SCREEN_TRANSP=1
        LV_FONT_MONTSERRAT_14=1
        LV_FONT_MONTSERRAT_16=1
        LV_FONT_MONTSERRAT_20=1
        LV_FONT_MONTSERRAT_28=1
        LV_FONT_MONTSERRAT_48=1
        LV_MEM_SIZE=262144
        LV_USE_LOG=0)
    # LVGL 8.4 triggers -Wunused-parameter etc. under -Wall -Wextra; keep our
    # warning gates for our own code only.
    target_compile_options(lvgl PRIVATE -w)

    # SDL2 built from source: the Homebrew SDL2 on this machine is x86_64-only
    # while the host is arm64 (see docs/TOOLCHAIN.md).
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-2.30.11
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL2)
endif()

# Warnings for our code only; third-party targets above were created before
# this directory-scoped option takes effect.
add_compile_options(-Wall -Wextra)

add_subdirectory(components/common)
add_subdirectory(components/domain)
add_subdirectory(components/protocol)
add_subdirectory(components/application)
add_subdirectory(components/persistence)
add_subdirectory(components/remote_core)
add_subdirectory(simulator/scorer-cli)

if(PADEL_BUILD_UI)
    add_subdirectory(components/ui)
    add_subdirectory(simulator/court-sim)
endif()

if(PADEL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
