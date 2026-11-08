# GPIO/buzzer probe. A separate tiny app, flashed into the hub's spare OTA
# slot with otatool and booted from there; the hub app in the other slot is untouched.
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gpio_probe)
