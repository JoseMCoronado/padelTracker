cmake_minimum_required(VERSION 3.16)

# Shared native/firmware components (domain, application, persistence, ui, ...).
list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../components)

# Only build main and its dependency closure, not every shared component.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(padel_court_display)
