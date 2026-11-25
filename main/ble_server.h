/**
 * @file    ble_server.h
 * @brief   BLE GATT 服务：接收手机下发的数据。
 * @details ESP32 作为 BLE Peripheral（从机），手机作为 Central（主机）连接。
 *
 *  GATT 结构：
 *
 *    服务 UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E  （Nordic UART 风格，手机端生态成熟）
 *     ├─ RX 特征 6E400002-...  属性: Write / Write Without Response
 *     │    手机 → ESP32：下发图像数据与指令
 *     └─ TX 特征 6E400003-...  属性: Notify
 *          ESP32 → 手机：状态回传
 *
 *  为什么用这组 UUID：
 *    它与 Nordic UART Service (NUS) 兼容，很多手机 BLE 调试 App
 *    （如 nRF Connect、Serial Bluetooth Terminal）可以直接识别，
 *    便于你在写 App 之前先用现成工具测试。
 */
#ifndef BLE_SERVER_H
#define BLE_SERVER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* ==================== 数据回调 ==================== */

/**
 * @brief 收到手机下发数据时的回调。
 * @param data 数据指针
 * @param len  数据长度
 * @note  回调在 BLE 主机任务上下文执行，**不要在里面做耗时操作**
 *        （比如直接写 UART 发送大块数据），否则会阻塞协议栈导致断连。
 *        正确做法是拷进队列，由独立任务处理。
 */
typedef void (*ble_server_rx_cb_t)(const uint8_t *data, size_t len);

/* ==================== 接口 ==================== */

/**
 * @brief  初始化并启动 BLE 服务。
 * @param  rx_cb 收到数据时的回调
 * @retval ESP_OK 成功
 * @note   需要先初始化 NVS（ble_server_init 内部会处理）。
 */
esp_err_t ble_server_init(ble_server_rx_cb_t rx_cb);

/**
 * @brief 向手机发送通知（状态回传）。
 * @param data 数据
 * @param len  长度
 * @retval ESP_OK 成功
 * @retval ESP_ERR_INVALID_STATE 当前无连接
 */
esp_err_t ble_server_notify(const uint8_t *data, size_t len);

/**
 * @brief 查询当前是否有手机连接。
 * @retval 1 已连接，0 未连接
 */
int ble_server_is_connected(void);

#endif /* BLE_SERVER_H */
