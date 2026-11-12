cmake_minimum_required(VERSION 3.16)

# Xiaozhi-VN ZClaw Edition (ESP32-S3 SuperMini - 4MB Flash)
# Thêm thư mục components cục bộ (minimp3 single-header decoder)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi_vn_zclaw)

