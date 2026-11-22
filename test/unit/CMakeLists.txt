# On-target unit tests (Unity). Collects components/*/test/test_*.c.
#   idf.py -C test/unit -p /dev/ttyACM0 flash monitor
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wp_unit_tests)
