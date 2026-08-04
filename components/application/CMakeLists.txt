if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/court_service.cpp" "src/pairing.cpp"
        INCLUDE_DIRS include
        REQUIRES common domain protocol)
else()
    add_library(padel_application STATIC
        src/court_service.cpp
        src/pairing.cpp)
    target_include_directories(padel_application PUBLIC include)
    target_link_libraries(padel_application PUBLIC padel_common padel_domain padel_protocol)
endif()
