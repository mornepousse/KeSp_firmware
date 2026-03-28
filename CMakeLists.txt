# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Board selection: idf.py -DBOARD=kase_v1 build
if(NOT DEFINED BOARD)
    set(BOARD "kase_v2_debug" CACHE STRING "Target board")
endif()
set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}" CACHE INTERNAL "")
if(NOT EXISTS "${BOARD_DIR}/board.h")
    message(FATAL_ERROR "Board '${BOARD}' not found: ${BOARD_DIR}/board.h does not exist")
endif()

# Detect display backend from board.h
file(READ "${BOARD_DIR}/board.h" _board_h_content)
if(_board_h_content MATCHES "BOARD_DISPLAY_BACKEND_ROUND")
    set(DISPLAY_BACKEND "round" CACHE INTERNAL "")
else()
    set(DISPLAY_BACKEND "oled" CACHE INTERNAL "")
endif()

# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(KeSp)
