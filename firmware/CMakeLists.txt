cmake_minimum_required(VERSION 3.16.0)
# Firmware version comes from version.txt. Declare it as a configure dependency so that editing it alone
# re-runs CMake and refreshes the version baked into the app descriptor.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/version.txt")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hermes_voice_box)
