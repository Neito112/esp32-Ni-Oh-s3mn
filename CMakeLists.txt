cmake_minimum_required(VERSION 3.16)

# S.W.Ni-Oh (ESP32-S3 SuperMini - 4MB Flash)
# Thêm thư mục components cục bộ
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/.pio/deps" AND DEFINED IDF_COMPONENT_MANAGER AND "${IDF_COMPONENT_MANAGER}" STREQUAL "0")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/.pio/deps")
    set(ENV{IDF_COMPONENT_MANAGER} "0")
    set(ENV{LVGL_VERSION} "8.4.0")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sw_nioh)

