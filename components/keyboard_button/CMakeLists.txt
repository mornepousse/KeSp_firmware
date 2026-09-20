idf_component_register(SRC_DIRS "src" "."
                        INCLUDE_DIRS "include"
                        REQUIRES driver esp_timer)
# Board-specific config (matrix scanning delays)
target_include_directories(${COMPONENT_LIB} PRIVATE "${BOARD_DIR}")

include(package_manager)
cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
