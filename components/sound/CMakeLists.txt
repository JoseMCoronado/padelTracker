if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/tones.cpp"
        INCLUDE_DIRS include)
else()
    add_library(padel_sound STATIC src/tones.cpp)
    target_include_directories(padel_sound PUBLIC include)
    target_compile_features(padel_sound PUBLIC cxx_std_17)
endif()
