if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/crc16.cpp" "src/packets.cpp" "src/dedup.cpp"
        INCLUDE_DIRS include
        REQUIRES common)
else()
    add_library(padel_protocol STATIC
        src/crc16.cpp
        src/packets.cpp
        src/dedup.cpp)
    target_include_directories(padel_protocol PUBLIC include)
    target_link_libraries(padel_protocol PUBLIC padel_common)
endif()
