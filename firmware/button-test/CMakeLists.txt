cmake_minimum_required(VERSION 3.16)

# Only build main and its dependency closure; this app needs no shared
# components (pure GPIO), which keeps the build to a few seconds.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(button_test)
