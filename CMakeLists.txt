cmake_minimum_required(VERSION 3.22)

# Panel board: waveshare43 (default) or wt32sc01plus. Use one build directory per board:
#   idf.py build                                              (Waveshare, build/, sdkconfig)
#   idf.py -B build-wt32sc01plus -D WP_BOARD=wt32sc01plus build   (sdkconfig.wt32sc01plus)
set(WP_BOARD "waveshare43" CACHE STRING "Panel board (boards/<name>.defaults)")
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${WP_BOARD}.defaults")
    message(FATAL_ERROR "Unknown WP_BOARD '${WP_BOARD}': no boards/${WP_BOARD}.defaults")
endif()
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;boards/${WP_BOARD}.defaults")
if(NOT WP_BOARD STREQUAL "waveshare43")
    set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${WP_BOARD}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(oh-wallpanel)
