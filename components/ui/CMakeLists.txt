if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/model_builder.cpp" "src/court_ui.cpp" "src/ui_helpers.cpp"
             "src/screen_live.cpp" "src/screen_setup.cpp" "src/screen_misc.cpp"
        INCLUDE_DIRS include
        PRIV_INCLUDE_DIRS src
        REQUIRES common domain protocol application lvgl)
else()
    add_library(padel_ui STATIC
        src/model_builder.cpp
        src/court_ui.cpp
        src/ui_helpers.cpp
        src/screen_live.cpp
        src/screen_setup.cpp
        src/screen_misc.cpp)
    target_include_directories(padel_ui PUBLIC include)
    target_include_directories(padel_ui PRIVATE src)
    target_link_libraries(padel_ui PUBLIC padel_common padel_domain padel_application lvgl)
endif()
