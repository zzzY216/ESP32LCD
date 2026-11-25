/**
 * @file    test_proto.c
 * @brief   ESP32 端 proto.c 的 PC 验证。
 * @details 重点验证：ESP32 生成的帧能被 STM32 端解析器正确接收。
 *          两边算法必须完全一致，否则通讯会失败。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "proto.h"

static int g_run = 0;
static int g_fail = 0;

#define CHECK(cond, msg, ...)                                      \
    do {                                                           \
        g_run++;                                                   \
        if (!(cond)) {                                             \
            g_fail++;                                              \
            printf("  [FAIL] " msg "\n", ##__VA_ARGS__);           \
        } else {                                                   \
            printf("  [ OK ] " msg "\n", ##__VA_ARGS__);           \
        }                                                          \
    } while (0)

/** 打印十六进制 */
static void dump(const char *tag, const uint8_t *b, size_t n)
{
    size_t i;
    printf("    %s: ", tag);
    for (i = 0; i < n; i++) {
        printf("%02X ", b[i]);
    }
    printf("\n");
}

int main(void)
{
    uint8_t buf[PROTO_MAX_FRAME];
    size_t  n;

    printf("=== ESP32 proto.c 验证 ===\n\n");

    /* ---- 1. CRC 基准值 ---- */
    {
        const uint8_t data[] = {'1','2','3','4','5','6','7','8','9'};
        uint16_t crc = proto_crc16(data, 9);

        printf("[CRC16]\n");
        CHECK(crc == 0x29B1u,
              "crc16(\"123456789\") = 0x%04X（基准 0x29B1）", crc);
    }

    /* ---- 2. PING 帧必须与 STM32 端文档一致 ---- */
    {
        /* STM32 端文档 docs/WIRING.md 里给出的测试帧：
         *   A5 5A 01 00 00 00 74 F2 */
        const uint8_t expect[] = {0xA5, 0x5A, 0x01, 0x00, 0x00, 0x00, 0x74, 0xF2};

        printf("\n[PING 帧与 STM32 文档一致]\n");
        n = proto_build(buf, sizeof(buf), PROTO_CMD_PING, 0x00, NULL, 0);

        dump("生成", buf, n);
        dump("期望", expect, sizeof(expect));

        CHECK(n == sizeof(expect), "帧长度 = %u（期望 %u）",
              (unsigned)n, (unsigned)sizeof(expect));
        CHECK(memcmp(buf, expect, sizeof(expect)) == 0, "字节完全一致");
    }

    /* ---- 3. IMAGE_BEGIN 负载布局 ---- */
    {
        printf("\n[IMAGE_BEGIN 负载]\n");
        n = proto_build_image_begin(buf, sizeof(buf), 1, 240, 240, 0, 0);

        CHECK(n == PROTO_HEADER_SIZE + 9 + PROTO_TAIL_SIZE,
              "整帧长度 = %u", (unsigned)n);
        CHECK(buf[2] == PROTO_CMD_IMAGE_BEGIN, "命令码 = 0x%02X", buf[2]);

        /* 负载：w(2) h(2) x(2) y(2) fmt(1)，小端 */
        CHECK(buf[6] == 240 && buf[7] == 0,
              "宽 = %u（小端 240）", (unsigned)(buf[6] | (buf[7] << 8)));
        CHECK(buf[8] == 240 && buf[9] == 0,
              "高 = %u", (unsigned)(buf[8] | (buf[9] << 8)));
        CHECK(buf[10] == 0 && buf[11] == 0, "x = 0");
        CHECK(buf[12] == 0 && buf[13] == 0, "y = 0");
        CHECK(buf[14] == 0, "format = RGB565");
    }

    /* ---- 4. 帧的自洽性：CRC 覆盖范围正确 ---- */
    {
        uint8_t payload[5] = {0x11, 0x22, 0x33, 0x44, 0x55};
        uint16_t calc;

        printf("\n[CRC 覆盖范围]\n");
        n = proto_build(buf, sizeof(buf), PROTO_CMD_IMAGE_DATA, 0x42,
                        payload, sizeof(payload));

        /* 手工按协议规定重算：CMD + SEQ + LEN(2) + PAYLOAD */
        calc = proto_crc16(&buf[2], 4 + sizeof(payload));

        CHECK(buf[n-2] == (uint8_t)(calc & 0xFF),
              "CRC 低字节 = 0x%02X", buf[n-2]);
        CHECK(buf[n-1] == (uint8_t)((calc >> 8) & 0xFF),
              "CRC 高字节 = 0x%02X", buf[n-1]);
    }

    /* ---- 5. 参数校验 ---- */
    {
        printf("\n[参数校验]\n");
        CHECK(proto_build(NULL, 100, PROTO_CMD_PING, 0, NULL, 0) == 0,
              "输出缓冲为 NULL 时返回 0");
        CHECK(proto_build(buf, 3, PROTO_CMD_PING, 0, NULL, 0) == 0,
              "缓冲不足时返回 0");
        /* 超长负载 */
        {
            static uint8_t big[PROTO_MAX_PAYLOAD + 1];
            CHECK(proto_build(buf, sizeof(buf), PROTO_CMD_IMAGE_DATA, 0,
                              big, sizeof(big)) == 0,
                  "负载超过上限时返回 0");
        }
    }

    /* ---- 6. 便捷函数 ---- */
    {
        printf("\n[便捷函数]\n");
        n = proto_build_u8(buf, sizeof(buf), PROTO_CMD_SET_SCALE, 5, 2);
        CHECK(n == PROTO_HEADER_SIZE + 1 + PROTO_TAIL_SIZE,
              "SET_SCALE 帧长度 = %u", (unsigned)n);
        CHECK(buf[6] == 2, "缩放值 = %u", buf[6]);

        n = proto_build_u16(buf, sizeof(buf), PROTO_CMD_CLEAR, 6, 0xF800);
        CHECK(buf[6] == 0x00 && buf[7] == 0xF8,
              "颜色小端 0xF800 -> [%02X %02X]", buf[6], buf[7]);
    }

    /* ---- 7. 大负载边界 ---- */
    {
        static uint8_t big[PROTO_MAX_PAYLOAD];

        printf("\n[最大负载]\n");
        memset(big, 0xAA, sizeof(big));
        n = proto_build(buf, sizeof(buf), PROTO_CMD_IMAGE_DATA, 7,
                        big, sizeof(big));

        CHECK(n == PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD + PROTO_TAIL_SIZE,
              "1024 字节负载帧长度 = %u", (unsigned)n);
    }

    printf("\n=== %d 项检查，%d 项失败 ===\n", g_run, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
