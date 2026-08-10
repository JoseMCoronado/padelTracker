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
    domain/test_engine.cpp
    domain/test_club_round.cpp)
target_link_libraries(domain_tests PRIVATE padel_domain Catch2::Catch2WithMain)
target_include_directories(domain_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(domain_tests)

add_executable(common_tests
    common/test_log.cpp)
target_link_libraries(common_tests PRIVATE padel_common Catch2::Catch2WithMain)
catch_discover_tests(common_tests)

add_executable(protocol_tests
    protocol/test_crc16.cpp
    protocol/test_packets.cpp
    protocol/test_pairing_packets.cpp
    protocol/test_dedup.cpp)
target_link_libraries(protocol_tests PRIVATE padel_protocol Catch2::Catch2WithMain)
catch_discover_tests(protocol_tests)

add_executable(application_tests
    application/test_court_service.cpp
    application/test_pairing.cpp
    application/test_observability.cpp
    application/test_club.cpp)
target_link_libraries(application_tests PRIVATE padel_application Catch2::Catch2WithMain)
catch_discover_tests(application_tests)

add_executable(persistence_tests
    persistence/test_journal.cpp
    persistence/test_power_loss.cpp)
target_link_libraries(persistence_tests PRIVATE padel_persistence padel_application
                                                Catch2::Catch2WithMain)
catch_discover_tests(persistence_tests)

add_executable(integration_tests
    integration/test_radio_sim.cpp
    integration/test_remote_e2e.cpp)
target_link_libraries(integration_tests PRIVATE padel_persistence padel_application
                                                padel_remote_core Catch2::Catch2WithMain)
target_include_directories(integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(integration_tests)

add_executable(remote_tests
    remote/test_remote_core.cpp)
target_link_libraries(remote_tests PRIVATE padel_remote_core Catch2::Catch2WithMain)
catch_discover_tests(remote_tests)

if(PADEL_BUILD_UI)
    add_executable(ui_tests
        ui/test_view_model.cpp
        ui/test_ui_render.cpp)
    target_link_libraries(ui_tests PRIVATE padel_ui Catch2::Catch2WithMain)
    target_include_directories(ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(ui_tests)
endif()
