add_executable(court-sim
    main.cpp
    sdl_backend.cpp)
# The club list is read from the source tree so editing it needs no rebuild;
# baking the path in means the sim works from any working directory.
# PADEL_PLAYERS_FILE in the environment overrides it.
target_compile_definitions(court-sim PRIVATE
    PADEL_CLUB_LIST_FILE="${CMAKE_SOURCE_DIR}/config/players.txt")
target_link_libraries(court-sim PRIVATE
    padel_ui
    padel_application
    padel_persistence
    padel_remote_core
    padel_sound
    lvgl
    SDL2::SDL2-static)
