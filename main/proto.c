/**
 * @file    proto.c
 * @brief   协议封装实现（与 STM32 端算法逐字节一致）。
 */
#include "proto.h"
#include <string.h>

/**
 * @brief CRC-16/CCITT-FALSE。
 * @details 多项式 0x1021，初值 0xFFFF，输入/输出均不反转，结果不异或。
 *
 * 【验证】crc16("123456789", 9) 必须等于 0x29B1。
 *         如果这个值不对，说明算法参数错了，STM32 会一直回 NAK。
 */
uint16_t proto_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    size_t   i;
    uint8_t  bit;

    if (data == NULL) {
        return crc;
    }

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

size_t proto_build(uint8_t *out, size_t out_cap,
                   uint8_t cmd, uint8_t seq,
                   const uint8_t *payload, size_t len)
{
    size_t   total;
    uint16_t crc;

    if (out == NULL || len > PROTO_MAX_PAYLOAD) {
        return 0;
    }

    total = PROTO_HEADER_SIZE + len + PROTO_TAIL_SIZE;
    if (out_cap < total) {
        return 0;
    }

    /* 帧头 */
    out[0] = PROTO_SYNC0;
    out[1] = PROTO_SYNC1;
    out[2] = cmd;
    out[3] = seq;
    out[4] = (uint8_t)(len & 0xFFu);
    out[5] = (uint8_t)((len >> 8) & 0xFFu);

    /* 负载 */
    if (payload != NULL && len > 0) {
        memcpy(&out[PROTO_HEADER_SIZE], payload, len);
    } else if (len > 0) {
        memset(&out[PROTO_HEADER_SIZE], 0, len);
    }

    /* CRC 覆盖 CMD 到 PAYLOAD 末尾 */
    crc = proto_crc16(&out[2], 4u + len);
    out[PROTO_HEADER_SIZE + len]     = (uint8_t)(crc & 0xFFu);
    out[PROTO_HEADER_SIZE + len + 1] = (uint8_t)((crc >> 8) & 0xFFu);

    return total;
}

size_t proto_build_image_begin(uint8_t *out, size_t cap, uint8_t seq,
                               uint16_t w, uint16_t h, uint16_t x, uint16_t y)
{
    uint8_t p[9];

    /* 小端序：低字节在前 */
    p[0] = (uint8_t)(w & 0xFFu);
    p[1] = (uint8_t)((w >> 8) & 0xFFu);
    p[2] = (uint8_t)(h & 0xFFu);
    p[3] = (uint8_t)((h >> 8) & 0xFFu);
    p[4] = (uint8_t)(x & 0xFFu);
    p[5] = (uint8_t)((x >> 8) & 0xFFu);
    p[6] = (uint8_t)(y & 0xFFu);
    p[7] = (uint8_t)((y >> 8) & 0xFFu);
    p[8] = 0u;      /* format = RGB565 */

    return proto_build(out, cap, PROTO_CMD_IMAGE_BEGIN, seq, p, sizeof(p));
}

size_t proto_build_u8(uint8_t *out, size_t cap, uint8_t cmd, uint8_t seq, uint8_t value)
{
    return proto_build(out, cap, cmd, seq, &value, 1u);
}

size_t proto_build_u16(uint8_t *out, size_t cap, uint8_t cmd, uint8_t seq, uint16_t value)
{
    uint8_t p[2];
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
    return proto_build(out, cap, cmd, seq, p, 2u);
}
