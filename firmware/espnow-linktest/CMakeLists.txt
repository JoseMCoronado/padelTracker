cmake_minimum_required(VERSION 3.16)

# Shared native/firmware components (protocol, common, ...).
list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../components)

# Only build main and its dependency closure, not every shared component.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espnow_linktest)
