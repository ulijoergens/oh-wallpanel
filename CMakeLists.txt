cmake_minimum_required(VERSION 3.22)

# Panel board: waveshare43 (default), wt32sc01plus or esp32-2424s012. One build directory
# per board:
#   idf.py build                                                  (Waveshare, build/, sdkconfig)
#   idf.py -B build-wt32sc01plus -D WP_BOARD=wt32sc01plus build       (sdkconfig.wt32sc01plus)
#   idf.py -B build-esp32-2424s012 -D WP_BOARD=esp32-2424s012 build   (sdkconfig.esp32-2424s012)
set(WP_BOARD "waveshare43" CACHE STRING "Panel board (boards/<name>.defaults)")
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${WP_BOARD}.defaults")
    message(FATAL_ERROR "Unknown WP_BOARD '${WP_BOARD}': no boards/${WP_BOARD}.defaults")
endif()
if(WP_BOARD STREQUAL "esp32-2424s012")
    set(WP_CHIP "esp32c3")
else()
    set(WP_CHIP "esp32s3")
endif()
set(IDF_TARGET "${WP_CHIP}")
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;boards/${WP_CHIP}.defaults;boards/${WP_BOARD}.defaults")
if(NOT WP_BOARD STREQUAL "waveshare43")
    set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${WP_BOARD}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(NOT WP_CHIP STREQUAL "esp32s3")
    # The component manager's lock file is per chip (dependencies.lock = ESP32-S3).
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${WP_CHIP}")
endif()
project(oh-wallpanel)
