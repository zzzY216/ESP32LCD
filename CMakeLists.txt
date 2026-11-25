# ESP32 BLE → STM32 图像转发项目
#
# 该文件必须保持这个结构：先 include project.cmake，再调用 project()。

cmake_minimum_required(VERSION 3.22)

# 蓝牙专用 mbedTLS 配置（省 RAM，ESP-IDF 官方示例都这么做）
cmake_path(CONVERT "$ENV{IDF_PATH}" TO_CMAKE_PATH_LIST ESP_IDF_PATH)
list(APPEND sdkconfig_defaults "${ESP_IDF_PATH}/components/mbedtls/config/mbedtls_preset_bt.conf")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 只编译实际用到的组件，显著缩短构建时间、减小固件体积
idf_build_set_property(MINIMAL_BUILD ON)

project(olcd_esp32)
