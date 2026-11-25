/**
 * @file    main.c
 * @brief   ESP32 主程序：BLE 收数据 → 打包成协议帧 → UART 发给 STM32。
 *
 * 数据流：
 *
 *     手机 App ──BLE Write──► ESP32 ──UART2──► STM32 ──SPI──► ST7789 屏幕
 *
 * 架构要点：
 *
 *   1. BLE 回调只做"拷贝进队列"，绝不做 UART 发送。
 *      因为 BLE 回调运行在协议栈任务里，阻塞会导致断连。
 *
 *   2. 独立的 forward 任务从队列取数据，打包成协议帧发给 STM32。
 *
 *   3. 手机端可以直接下发"完整的协议帧"，ESP32 原样透传；
 *      也可以下发"自定义格式"，由 ESP32 转换（见 handle_packet）。
 */
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ble_server.h"
#include "uart_link.h"
#include "proto.h"

static const char *TAG = "main";

/* ==================== 配置 ==================== */

/**
 * @brief 手机下发数据的分片大小。
 * @details 一帧 240x240 RGB565 图像 = 115200 字节。
 *          按 512 字节分片，约需 225 片。
 *          片间留 2ms 给 STM32 喘息，总耗时约 0.5 秒。
 */
#define FRAME_CHUNK_SIZE        512u

/** 片间延时（毫秒）。STM32 是软件 SPI，太快会导致其接收缓冲溢出 */
#define CHUNK_GAP_MS            2u

/** 队列深度：容纳 BLE 突发数据 */
#define RX_QUEUE_DEPTH          32

/**
 * @brief 协议帧重组缓冲区大小。
 *
 * @details 【为什么必须要有这个 —— 这是实测踩到的真实 bug】
 *
 *   BLE 单次写入的载荷上限是 **MTU - 3**。协商到 MTU=512 时，
 *   每次最多只能发 **509 字节**。
 *
 *   而手机下发的一个 IMAGE_DATA 帧大小是：
 *       帧头 6 + 负载 512 + CRC 2 = **520 字节**
 *
 *   520 > 509，所以手机端的 GATT 栈会自动把它拆成两包：
 *       第 1 包：509 字节（帧头 + 大部分负载）
 *       第 2 包： 11 字节（剩余负载 + CRC）
 *
 *   如果 ESP32 把每个 BLE 包都当成"一条独立完整的协议帧"直接透传，
 *   就会出现：
 *       - 第 1 包：以 A5 5A 开头 → 被当成完整帧透传（但少了 11 字节）
 *       - 第 2 包：以数据字节开头 → 认不出帧头 → 被当成"裸图像数据"
 *                 重新封装成新的 IMAGE_DATA 帧发下去
 *
 *   结果 STM32 收到的是**错乱的数据流**：
 *       [残缺的帧][ESP32 凭空造的帧][残缺的帧]...
 *   STM32 的 CRC 校验会大面积失败，表现为"完全控制不了"。
 *
 *   日志里的典型症状就是：
 *       frame passthrough: cmd=0x11 seq=138 len=509   <- 509，差 11 字节
 *       RX 11 bytes: EE 8B EF 33 ...                   <- 这是缺少的尾巴
 *       not a frame (head=74 AD), treated as raw data  <- 被误处理
 *
 *   修复办法：在这里做**流式帧重组**。收到的字节先追加到累积缓冲，
 *   然后按协议格式解析出完整帧（知道 LEN 就能算出总长），
 *   只有凑齐一整个帧才交给后续处理。没凑齐就等下一包。
 */
#define RX_ACCUM_SIZE           (PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD + PROTO_TAIL_SIZE)

/* ==================== 内部状态 ==================== */

/** BLE 收到的数据块 */
typedef struct {
    uint16_t len;
    uint8_t  data[512];
} rx_item_t;

static QueueHandle_t s_rx_queue = NULL;

/**
 * @brief 帧重组累积缓冲。
 * @details 大小要能容纳一个最大帧：6 + 1024 + 2 = 1032 字节。
 */
static uint8_t  s_accum[RX_ACCUM_SIZE];
static size_t   s_accum_len = 0;

/** 全局序号，每发一帧递增 */
static uint8_t s_seq = 0;

/** 统计 */
static uint32_t s_pkt_count   = 0;
static uint32_t s_byte_count  = 0;

/* ==================== 前向声明 ==================== */

static void ble_rx_callback(const uint8_t *data, size_t len);
static void forward_task(void *arg);
static void send_frame(uint8_t cmd, uint8_t seq, const uint8_t *payload, size_t len);

/**
 * @brief 处理一个**已重组的完整协议帧**（长度由 LEN 字段保证）。
 * @param frame 完整帧起始地址（含帧头和 CRC）
 * @param total 整帧字节数
 */
static void process_frame(const uint8_t *frame, size_t total);

/**
 * @brief 把 BLE 收到的字节追加到重组缓冲，尽可能多地解析出完整帧。
 * @retval true  数据属于协议帧（已消费）
 * @retval false 完全不是帧（调用方可走裸数据兜底）
 */
static bool feed_accum(const uint8_t *data, size_t len);

/* ==================== BLE 回调 ==================== */

/**
 * @brief BLE 收到手机数据的回调。
 * @details 【必须极快返回】只把数据拷进队列就返回，
 *          UART 发送交给 forward_task 处理。
 *
 *          这里会打印收到的原始字节（最多前 8 个），
 *          是排查手机端发送格式最快的手段：
 *            二进制帧 -> A5 5A 01 00 ...
 *            ASCII 文本 -> 41 35 20 35 ...（'A' '5' ' ' '5'）
 */
static void ble_rx_callback(const uint8_t *data, size_t len)
{
    rx_item_t item;

    if (data == NULL || len == 0) {
        return;
    }

    /* 调试日志：打印收到的前几个字节，便于确认格式 */
    ESP_LOGI(TAG, "RX %u bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
             (unsigned)len,
             len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
             len > 2 ? data[2] : 0, len > 3 ? data[3] : 0,
             len > 4 ? data[4] : 0, len > 5 ? data[5] : 0,
             len > 6 ? data[6] : 0, len > 7 ? data[7] : 0);

    if (len > sizeof(item.data)) {
        ESP_LOGW(TAG, "rx too long: %u, dropped", (unsigned)len);
        return;
    }

    item.len = (uint16_t)len;
    memcpy(item.data, data, len);

    /* 非阻塞入队；队列满说明 forward_task 处理不过来 */
    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "rx queue full, dropped %u bytes", (unsigned)len);
    }
}

/* ==================== UART 发送 ==================== */

/**
 * @brief 发送一帧到 STM32。
 */
static void send_frame(uint8_t cmd, uint8_t seq, const uint8_t *payload, size_t len)
{
    uint8_t buf[PROTO_MAX_FRAME];
    size_t  n;

    n = proto_build(buf, sizeof(buf), cmd, seq, payload, len);
    if (n == 0) {
        ESP_LOGE(TAG, "build frame failed (cmd=0x%02X len=%u)", cmd, (unsigned)len);
        return;
    }

    esp_err_t ret = uart_link_send(buf, n);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart send failed: %s", esp_err_to_name(ret));
    }
}

/**
 * @brief 把一段图像数据发给 STM32（分片为 IMAGE_DATA 帧）。
 */
static void send_image_data(const uint8_t *data, size_t len)
{
    size_t offset = 0;

    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > FRAME_CHUNK_SIZE) {
            chunk = FRAME_CHUNK_SIZE;
        }

        send_frame(PROTO_CMD_IMAGE_DATA, s_seq++, data + offset, chunk);
        offset += chunk;

        /* 给 STM32 喘息：它是软件 SPI 写屏，接收缓冲只有 512 字节 */
        if (offset < len) {
            uart_link_delay_ms(CHUNK_GAP_MS);
        }
    }
}

/* ==================== 数据处理 ==================== */

/**
 * @brief 判断一个字节是否是十六进制字符或分隔符。
 */
static int is_hex_char(uint8_t c)
{
    return ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')) ? 1 : 0;
}

static int hex_val(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * @brief 把「十六进制 ASCII 文本」解析成真正的字节数组。
 *
 * @details 【为什么需要这个】
 *
 *   很多 BLE 调试 App（包括 nRF Connect 的默认写界面）输入框只有文本模式，
 *   你在里面敲 "A5 5A 01 00" 时，App 会把每个字符按 UTF-8 发出来：
 *
 *     发送的 23 个字节： 0x41('A') 0x35('5') 0x20(' ') ...
 *     而不是期望的 8 个字节： 0xA5 0x5A 0x01 0x00 ...
 *
 *   第一字节是 0x41 而不是 0xA5，帧头匹配失败，STM32 会拒绝整帧。
 *
 *   本函数让 ESP32 主动兼容这种输入：检测到全是十六进制字符和空格，
 *   就自动转成二进制再处理。这样手机端不用改任何设置就能用。
 *
 * @param in      输入（可能含空格、逗号、0x 前缀、换行）
 * @param in_len  输入长度
 * @param out     输出缓冲区
 * @param out_cap 输出容量
 * @retval >=0    解析出的字节数；返回 0 表示不是十六进制文本
 */
static int parse_hex_text(const uint8_t *in, size_t in_len,
                          uint8_t *out, size_t out_cap)
{
    size_t  i = 0;
    size_t  n = 0;
    int     hi = -1;        /* 已读到的半字节，-1 表示还没有 */

    if (in == NULL || out == NULL || in_len == 0) {
        return 0;
    }

    /* 先做一次快速判断：整段必须只由十六进制字符和分隔符组成，
     * 且至少要有一个十六进制字符。否则不是 hex 文本。
     *
     * 注意 "0x" 前缀的 'x' 也要放行，否则 "0xA5" 这种写法会被拒绝。 */
    {
        size_t hex_count = 0;
        size_t bad_chars = 0;

        for (i = 0; i < in_len; i++) {
            uint8_t c = in[i];
            if (is_hex_char(c)) {
                hex_count++;
            } else if (c == ' ' || c == ',' || c == '\r' || c == '\n' ||
                       c == '\t' || c == ':' || c == '-') {
                /* 分隔符，允许 */
            } else if (c == 'x' || c == 'X') {
                /* 0x 前缀里的 x，允许 */
            } else {
                bad_chars++;
            }
        }

        /* 只要出现一个非 hex 非分隔符的字符，就认为是真正的二进制数据。
         * 注意：二进制帧里几乎不可能恰好全是 ASCII 十六进制字符。 */
        if (bad_chars > 0 || hex_count < 2) {
            return 0;
        }
    }

    /* 逐字符解析 */
    for (i = 0; i < in_len; i++) {
        uint8_t c = in[i];
        int v;

        /* 跳过 "0x" / "0X" 前缀。
         * 必须在这里显式识别，否则 '0' 会被当作十六进制数字 0，
         * 导致 "0xA5" 变成 0x0A、0x5? 这种错位。 */
        if (c == '0' && (i + 1) < in_len && (in[i + 1] == 'x' || in[i + 1] == 'X')) {
            i++;            /* 跳过 'x'，'0' 也一并跳过 */
            continue;
        }

        if (!is_hex_char(c)) {
            /* 分隔符：直接跳过，不清 hi，使连写与带空格写法等价 */
            continue;
        }

        v = hex_val(c);
        if (v < 0) {
            return 0;
        }

        if (hi < 0) {
            hi = v;
        } else {
            if (n >= out_cap) {
                return (int)n;      /* 输出满了，返回已解析的部分 */
            }
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }

    /* 结尾不该剩下半字节 */
    if (hi >= 0) {
        return 0;
    }

    return (int)n;
}

/**
 * @brief 处理一个来自手机的数据块（只负责"文本 → 二进制"的转换）。
 *
 * @details 【职责已调整】
 *
 *   原来这里做了三件事：hex 文本转换、帧透传、裸数据兜底。
 *   现在**帧透传已移到 feed_accum()/process_frame()**，因为它们需要
 *   跨 BLE 包重组完整帧（BLE 单包 509 字节装不下 520 字节的帧）。
 *
 *   本函数只保留两件事：
 *
 *   1. 十六进制文本 -> 自动解析成字节
 *      兼容 nRF Connect 等只能输入文本的 App
 *
 *   2. 其他情况 -> 当作图像裸数据，分片成 IMAGE_DATA
 *      （用于"手机直接发像素"的简化模式）
 *
 * @note 调用者：ble_rx_callback 之后先走本函数做文本转换，
 *       若结果是二进制帧再交由 feed_accum 重组。
 */
static void handle_packet(const uint8_t *data, size_t len)
{
    uint8_t decoded[512];
    int     decoded_len;

    /* ---- 第 1 层：尝试按十六进制文本解析 ---- */
    decoded_len = parse_hex_text(data, len, decoded, sizeof(decoded));
    if (decoded_len > 0) {
        ESP_LOGI(TAG, "hex text detected (%u chars) -> %d bytes",
                 (unsigned)len, decoded_len);

        /* 转换后用解析结果继续处理 */
        data = decoded;
        len  = (size_t)decoded_len;
    }

    /* ---- 第 2 层：交给帧重组器 ----
     *
     * 不管是不是完整帧都交给 feed_accum：
     *   - 是帧头且收齐了  -> 立即透传
     *   - 是帧头但没收齐  -> 缓存起来等下一包
     *   - 完全不是帧      -> 返回 false，下面走裸数据兜底
     */
    if (feed_accum(data, len)) {
        return;         /* 已被重组器消费 */
    }

    /* ---- 第 3 层：图像裸数据 ---- */
    ESP_LOGW(TAG, "not a frame (head=%02X %02X), treated as raw image data",
             len > 0 ? data[0] : 0, len > 1 ? data[1] : 0);
    send_image_data(data, len);
}

/* ==================== 帧重组 ==================== */

/**
 * @brief 把一段 BLE 收到的字节追加到重组缓冲，并尽可能多地解析出完整帧。
 *
 * @details 【为什么必须做这一步】
 *
 *   手机下发的一个 IMAGE_DATA 帧 = 6(头) + 512(负载) + 2(CRC) = 520 字节，
 *   但 BLE 单次写入上限是 MTU-3 = 509 字节（MTU 协商到 512 时）。
 *   所以一帧必然被拆成 509 + 11 两包。
 *
 *   如果不重组，第二包（11 字节）认不出帧头，会被当成"裸图像数据"
 *   重新封装，导致发给 STM32 的数据流完全错乱 —— 这就是"控制不了"的根因。
 *
 *   本函数按协议格式判断帧边界：
 *       读到至少 6 字节 → 能知道 LEN → 算出总长 = 6 + LEN + 2
 *       等累积够了总长 → 完整帧，交给 process_frame()
 *       不够 → 保留在缓冲里等下一包
 *
 * @param data 新收到的字节
 * @param len  长度
 * @retval true  数据是协议帧（已消费，可能已透传或缓存）
 * @retval false 数据完全不是帧（调用方可以走裸数据兜底）
 */
static bool feed_accum(const uint8_t *data, size_t len)
{
    size_t   total;
    size_t   need;
    size_t   consumed;
    uint16_t payload_len;
    bool     saw_frame = false;

    if (data == NULL || len == 0) {
        return false;
    }

    /* 1. 追加到累积缓冲。若会溢出，说明协议流已经乱掉，整体丢弃重来。 */
    if (s_accum_len + len > sizeof(s_accum)) {
        ESP_LOGW(TAG, "accum overflow (%u + %u > %u), reset",
                 (unsigned)s_accum_len, (unsigned)len, (unsigned)sizeof(s_accum));
        s_accum_len = 0;
        /* 溢出了就只保留本包（本包自身不会超过 512，必然能放下） */
        if (len > sizeof(s_accum)) {
            len = sizeof(s_accum);
        }
    }
    memcpy(s_accum + s_accum_len, data, len);
    s_accum_len += len;

    /* 2. 循环从缓冲头部解析完整帧 */
    consumed = 0;
    for (;;) {
        size_t   avail = s_accum_len - consumed;
        uint8_t *p     = s_accum + consumed;

        /* 2.1 找帧头 A5 5A。找不到就丢掉前面的噪声字节。 */
        if (avail < 2) {
            break;                  /* 至少要有 2 字节才能判断帧头 */
        }
        if (p[0] != PROTO_SYNC0 || p[1] != PROTO_SYNC1) {
            /* 逐字节前移，直到遇到可能的帧头或剩余不足 2 字节 */
            size_t skip = 1;
            while (skip + 1 < avail &&
                   !(p[skip] == PROTO_SYNC0 && p[skip + 1] == PROTO_SYNC1)) {
                skip++;
            }
            if (consumed == 0 && !saw_frame) {
                /* 连开头都不是帧头，说明这批数据根本不属于协议流，
                 * 让调用方走裸数据兜底（先把刚追加的字节撤回来）。 */
                if (s_accum_len >= len) {
                    s_accum_len -= len;
                } else {
                    s_accum_len = 0;
                }
                return false;
            }
            ESP_LOGW(TAG, "skip %u bytes (no sync)", (unsigned)skip);
            consumed += skip;
            continue;
        }

        /* 2.2 有帧头了，但还需要 6 字节才能读出 LEN */
        if (avail < PROTO_HEADER_SIZE) {
            saw_frame = true;       /* 已确认是帧，等更多数据 */
            break;
        }

        payload_len = (uint16_t)(p[4] | ((uint16_t)p[5] << 8));

        /* 2.3 长度合法性检查 */
        if (payload_len > PROTO_MAX_PAYLOAD) {
            ESP_LOGW(TAG, "bad LEN=%u, drop 1 byte to resync", payload_len);
            consumed += 1;          /* 可能是假帧头，前移一个字节重找 */
            continue;
        }

        total = PROTO_HEADER_SIZE + (size_t)payload_len + PROTO_TAIL_SIZE;

        /* 2.4 还没收齐，等下一包 */
        if (avail < total) {
            saw_frame = true;
            break;
        }

        /* 2.5 收到完整帧了 —— 交给真正的处理逻辑 */
        saw_frame = true;
        process_frame(p, total);
        consumed += total;
    }

    /* 3. 把已消费的部分从缓冲里移除 */
    if (consumed > 0) {
        if (consumed >= s_accum_len) {
            s_accum_len = 0;
        } else {
            need = s_accum_len - consumed;
            memmove(s_accum, s_accum + consumed, need);
            s_accum_len = need;
        }
    }

    return saw_frame;
}

/* ==================== 转发任务 ==================== */

static void forward_task(void *arg)
{
    rx_item_t item;
    uint8_t   status[32];
    size_t    n;

    (void)arg;

    ESP_LOGI(TAG, "forward task started");

    for (;;) {
        if (xQueueReceive(s_rx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        s_pkt_count++;
        s_byte_count += item.len;

        /* 【关键】走 handle_packet，它会先做 hex 文本转换，
         * 再交给 feed_accum 做跨包帧重组。
         *
         * 因为 BLE 单包上限 509 字节，而一个 IMAGE_DATA 帧是 520 字节，
         * 必然被拆成 509 + 11 两包。必须拼回完整帧再处理，
         * 否则碎片会被当成"裸图像数据"重新封装，导致数据流错乱。
         */
        handle_packet(item.data, item.len);

        /* 每处理 20 个包，向手机回传一次进度（走 BLE Notify） */
        if ((s_pkt_count % 20u) == 0u && ble_server_is_connected()) {
            n = (size_t)snprintf((char *)status, sizeof(status),
                                 "pkts=%lu bytes=%lu",
                                 (unsigned long)s_pkt_count,
                                 (unsigned long)s_byte_count);
            (void)ble_server_notify(status, n);
        }
    }
}

/**
 * @brief 处理一个已重组的完整协议帧。
 *
 * @details 走到这里说明帧长度已经由 LEN 字段保证（凑齐了才调用）。
 *          直接透传给 STM32，不做任何重新封装。
 */
static void process_frame(const uint8_t *frame, size_t total)
{
    ESP_LOGI(TAG, "frame passthrough: cmd=0x%02X seq=%u len=%u",
             frame[2], frame[3], (unsigned)total);

    esp_err_t ret = uart_link_send(frame, total);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "passthrough failed");
    }
}

/* ==================== 应用入口 ==================== */

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "=== ESP32 BLE -> STM32 bridge ===");
    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());

    /* 1. 创建接收队列 */
    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_item_t));
    if (s_rx_queue == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }

    /* 2. 初始化 UART（先初始化，确保后续有地方发数据） */
    ret = uart_link_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart init failed");
        return;
    }

    /* 3. 启动转发任务 */
    if (xTaskCreate(forward_task, "forward", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return;
    }

    /* 4. 启动 BLE 服务（最后启动，回调里会用到队列和 UART） */
    ret = ble_server_init(ble_rx_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ble init failed");
        return;
    }

    ESP_LOGI(TAG, "ready. Connect with BLE app, then send data.");
    ESP_LOGI(TAG, "  Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    ESP_LOGI(TAG, "  RX char UUID: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E (write)");
    ESP_LOGI(TAG, "  TX char UUID: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E (notify)");
}
