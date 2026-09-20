# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Board selection — two forms:
#   idf.py -DBOARD=kase_v1 build                     a board of this repository (boards/<name>/)
#   idf.py -DBOARD_DIR=/path/to/my_board build       a board kept OUTSIDE the repository
#     (same four files: board.h, board_keymap.c, board_layout.c, sdkconfig.defaults;
#      BOARD is then the folder's name unless given). The firmware is a dependency
#      the user pins; their board lives in their own repository.
if(DEFINED BOARD_DIR AND NOT BOARD_DIR STREQUAL "")
    if(NOT IS_ABSOLUTE "${BOARD_DIR}")
        set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${BOARD_DIR}")   # relative = from the project dir
    endif()
    if(NOT DEFINED BOARD)
        get_filename_component(BOARD "${BOARD_DIR}" NAME)
    endif()
    set(BOARD "${BOARD}" CACHE STRING "Target board" FORCE)
    # FORCE, explicitly: -D created a cache entry, and without FORCE set(CACHE)
    # drops the normal variable and keeps the old (relative) cache value.
    set(_board_dir_abs "${BOARD_DIR}")
    set(BOARD_DIR "${_board_dir_abs}" CACHE INTERNAL "" FORCE)
    message(STATUS "Out-of-tree board '${BOARD}': ${BOARD_DIR}")
else()
    if(NOT DEFINED BOARD)
        set(BOARD "kase_v2_debug" CACHE STRING "Target board")
    endif()
    set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}" CACHE INTERNAL "")
endif()
if(BOARD STREQUAL "_template")
    message(FATAL_ERROR "boards/_template is the template: create a board with scripts/new-board.sh <name>")
endif()
if(NOT EXISTS "${BOARD_DIR}/board.h")
    message(FATAL_ERROR "Board '${BOARD}' not found: ${BOARD_DIR}/board.h does not exist")
endif()

# Per-board sdkconfig defaults: boards/<name>/sdkconfig.defaults on top of the
# root sdkconfig.defaults. The folder is the whole registration of a board —
# nothing at the repository root names a board. Each board builds in its own
# build_<name>/ with -DSDKCONFIG=build_<name>/sdkconfig, so nothing leaks.
set(_PER_BOARD_DEFAULTS "${BOARD_DIR}/sdkconfig.defaults")
if(NOT EXISTS "${_PER_BOARD_DEFAULTS}")
    message(FATAL_ERROR "Board '${BOARD}': ${_PER_BOARD_DEFAULTS} is missing (create it, empty if the board needs no override)")
endif()
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults" "${_PER_BOARD_DEFAULTS}")
message(STATUS "Board defaults: ${_PER_BOARD_DEFAULTS}")

# Detect display backend from board.h
file(READ "${BOARD_DIR}/board.h" _board_h_content)
# A #define at the start of a line, not any mention: a comment listing the
# backends (the template does) must not select one.
if(_board_h_content MATCHES "\n#define[ \t]+BOARD_DISPLAY_BACKEND_MEMLCD")
    set(DISPLAY_BACKEND "memlcd" CACHE INTERNAL "")
elseif(_board_h_content MATCHES "\n#define[ \t]+BOARD_DISPLAY_BACKEND_ROUND")
    set(DISPLAY_BACKEND "round" CACHE INTERNAL "")
else()
    set(DISPLAY_BACKEND "oled" CACHE INTERNAL "")
endif()

# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(KeSp)
