if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/journal.cpp" "src/stdio_file_backend.cpp"
        INCLUDE_DIRS include
        REQUIRES common domain protocol application)
else()
    add_library(padel_persistence STATIC
        src/journal.cpp
        src/stdio_file_backend.cpp)
    target_include_directories(padel_persistence PUBLIC include)
    target_link_libraries(padel_persistence PUBLIC padel_common padel_domain padel_protocol
                                                   padel_application)
endif()
