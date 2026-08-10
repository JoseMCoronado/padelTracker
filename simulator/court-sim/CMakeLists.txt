add_executable(court-sim
    main.cpp
    sdl_backend.cpp)
target_link_libraries(court-sim PRIVATE
    padel_ui
    padel_application
    padel_persistence
    padel_remote_core
    padel_sound
    lvgl
    SDL2::SDL2-static)
