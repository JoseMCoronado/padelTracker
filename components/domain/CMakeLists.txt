if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/reducer.cpp" "src/match_engine.cpp" "src/projection.cpp"
             "src/club_round.cpp"
        INCLUDE_DIRS include
        REQUIRES common)
else()
    add_library(padel_domain STATIC
        src/reducer.cpp
        src/match_engine.cpp
        src/projection.cpp
        src/club_round.cpp)
    target_include_directories(padel_domain PUBLIC include)
    target_link_libraries(padel_domain PUBLIC padel_common)
endif()
