/**
 * @file    uart_link.h
 * @brief   ESP32 → STM32 串口发送层。
 * @details 使用 UART2（GPIO17 = TX，GPIO16 = RX）。
 *
 *  接线（与 STM32 端 docs/WIRING.md 对应）：
 *      ESP32 GPIO17 (TX2) ──► STM32 PA10 (RX)
 *      ESP32 GPIO16 (RX2) ◄── STM32 PA9  (TX)
 *      ESP32 GND          ─── STM32 GND
 */
#ifndef UART_LINK_H
#define UART_LINK_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* ==================== 引脚与参数 ==================== */

/** 发送引脚（接 STM32 的 RX） */
#define UART_LINK_TX_GPIO       17
/** 接收引脚（接 STM32 的 TX） */
#define UART_LINK_RX_GPIO       16
/** 使用的串口号（UART0 留给日志输出） */
#define UART_LINK_PORT          2
/** 波特率，必须与 STM32 端 app_link_init() 一致 */
#define UART_LINK_BAUD          921600
/** 发送缓冲区大小 */
#define UART_LINK_TX_BUF_SIZE   4096
/** 接收缓冲区大小 */
#define UART_LINK_RX_BUF_SIZE   1024

/* ==================== 接口 ==================== */

/**
 * @brief  初始化 UART2。
 * @retval ESP_OK 成功
 */
esp_err_t uart_link_init(void);

/**
 * @brief  发送原始字节。
 * @param  data 数据
 * @param  len  长度
 * @retval ESP_OK 成功
 * @note   阻塞直到数据全部写入硬件 FIFO/驱动缓冲。
 */
esp_err_t uart_link_send(const uint8_t *data, size_t len);

/**
 * @brief  读取一个字节（非阻塞）。
 * @param  out 输出字节
 * @retval 1 = 读到，0 = 无数据
 * @note   用于接收 STM32 回的 ACK/NAK。
 */
int uart_link_read_byte(uint8_t *out);

/**
 * @brief  等待一小段时间（毫秒），供数据片之间让 STM32 喘口气。
 * @note   STM32 是软件 SPI 写屏，速度有限，连续灌数据会导致其接收缓冲溢出。
 */
void uart_link_delay_ms(uint32_t ms);

#endif /* UART_LINK_H */
