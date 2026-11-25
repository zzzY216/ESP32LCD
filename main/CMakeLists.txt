# main 组件
#
# 依赖说明（ESP-IDF 6.x 的组件划分）：
#   bt              —— BLE 协议栈（NimBLE 主机）
#   nvs_flash       —— BLE 配对信息需要存在 NVS 里
#   esp_driver_uart —— UART 驱动（6.x 起从 driver 拆出来独立成组件）
#   esp_driver_gpio —— GPIO 驱动（UART 引脚复用需要）
#   esp_timer       —— 高精度计时

idf_component_register(
    SRCS
        "main.c"
        "ble_server.c"
        "uart_link.c"
        "proto.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        bt
        nvs_flash
        esp_driver_uart
        esp_driver_gpio
        esp_timer
)
