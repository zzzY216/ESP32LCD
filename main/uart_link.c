/**
 * @file    uart_link.c
 * @brief   UART2 发送/接收实现。
 */
#include "uart_link.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "uart_link";

esp_err_t uart_link_init(void)
{
    esp_err_t ret;
    const uart_config_t cfg = {
        .baud_rate  = UART_LINK_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* 安装驱动：发送用独立缓冲，接收环形缓冲留给 STM32 的应答 */
    ret = uart_driver_install(UART_LINK_PORT,
                              UART_LINK_RX_BUF_SIZE,
                              UART_LINK_TX_BUF_SIZE,
                              0,      /* 不使用事件队列 */
                              NULL,
                              0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(UART_LINK_PORT, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 绑定引脚：TX=GPIO17，RX=GPIO16，无流控引脚 */
    ret = uart_set_pin(UART_LINK_PORT,
                       UART_LINK_TX_GPIO,
                       UART_LINK_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 清空接收缓冲，避免上电残留数据干扰 */
    uart_flush_input(UART_LINK_PORT);

    ESP_LOGI(TAG, "UART%d ready: TX=GPIO%d RX=GPIO%d baud=%d",
             UART_LINK_PORT, UART_LINK_TX_GPIO, UART_LINK_RX_GPIO, UART_LINK_BAUD);
    return ESP_OK;
}

esp_err_t uart_link_send(const uint8_t *data, size_t len)
{
    int written;

    if (data == NULL || len == 0) {
        return ESP_OK;
    }

    written = uart_write_bytes(UART_LINK_PORT, (const char *)data, len);
    if (written < 0) {
        ESP_LOGE(TAG, "write failed: %d", written);
        return ESP_FAIL;
    }

    /* 等数据真正发出，避免调用方立刻覆盖缓冲区 */
    return uart_wait_tx_done(UART_LINK_PORT, pdMS_TO_TICKS(1000));
}

int uart_link_read_byte(uint8_t *out)
{
    if (out == NULL) {
        return 0;
    }
    return (uart_read_bytes(UART_LINK_PORT, out, 1, 0) == 1) ? 1 : 0;
}

void uart_link_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}
