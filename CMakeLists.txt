cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Test builds: idf.py -DHUB_TEST_HOOKS=1 build (adds /api/debug/crash for the boot-loop
# guard tests). The value is cached in build/: build releases with -DHUB_TEST_HOOKS=0.
if(HUB_TEST_HOOKS)
    idf_build_set_property(COMPILE_DEFINITIONS "HUB_TEST_HOOKS=1" APPEND)
endif()
project(esp32c5-zigbee-hub)
