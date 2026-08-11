if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/log.cpp" "src/battery.cpp" "src/idle_dim.cpp"
        INCLUDE_DIRS include)
else()
    add_library(padel_common STATIC src/log.cpp src/battery.cpp src/idle_dim.cpp)
    target_include_directories(padel_common PUBLIC include)
    target_compile_features(padel_common PUBLIC cxx_std_17)
endif()
