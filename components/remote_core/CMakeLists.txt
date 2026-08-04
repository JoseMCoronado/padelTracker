if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/remote_core.cpp"
        INCLUDE_DIRS include
        REQUIRES common protocol)
else()
    add_library(padel_remote_core STATIC
        src/remote_core.cpp)
    target_include_directories(padel_remote_core PUBLIC include)
    target_link_libraries(padel_remote_core PUBLIC padel_common padel_protocol)
endif()
