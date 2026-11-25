/**
 * @file    proto.h
 * @brief   ESP32 侧的协议封装（与 STM32 端完全一致）。
 * @details 帧格式：
 *
 *    +-------+-------+-------+-------+--------+--------+----------+-------+
 *    | SYNC0 | SYNC1 |  CMD  |  SEQ  | LEN_L  | LEN_H  | PAYLOAD  | CRC16 |
 *    | 0xA5  | 0x5A  | 1字节 | 1字节 |        |        | N 字节   | 2字节 |
 *    +-------+-------+-------+-------+--------+--------+----------+-------+
 *
 *  CRC16 = CRC-16/CCITT-FALSE，覆盖 CMD+SEQ+LEN+PAYLOAD
 *  基准校验：crc16("123456789", 9) == 0x29B1
 *
 *  【重要】本文件必须与 STM32 端 Hardware/proto.h 保持同步。
 *          任何一处改动都要两边同时改，否则通讯会失败。
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>
#include <stddef.h>

/* ==================== 协议参数 ==================== */

#define PROTO_SYNC0             0xA5u
#define PROTO_SYNC1             0x5Au

/** 单帧最大负载 */
#define PROTO_MAX_PAYLOAD       1024u

/** 帧头长度：SYNC0 + SYNC1 + CMD + SEQ + LEN(2) */
#define PROTO_HEADER_SIZE       6u

/** 帧尾长度：CRC16 */
#define PROTO_TAIL_SIZE         2u

/** 整帧最大长度 */
#define PROTO_MAX_FRAME         (PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD + PROTO_TAIL_SIZE)

/* ==================== 命令码（与 STM32 端一致） ==================== */

typedef enum {
    PROTO_CMD_PING          = 0x01u,
    PROTO_CMD_PONG          = 0x02u,

    PROTO_CMD_IMAGE_BEGIN   = 0x10u,
    PROTO_CMD_IMAGE_DATA    = 0x11u,
    PROTO_CMD_IMAGE_END     = 0x12u,

    PROTO_CMD_PLAY          = 0x20u,
    PROTO_CMD_PAUSE         = 0x21u,
    PROTO_CMD_STOP          = 0x22u,

    PROTO_CMD_SET_SCALE     = 0x30u,
    PROTO_CMD_SET_POS       = 0x31u,
    PROTO_CMD_SET_BRIGHT    = 0x32u,
    PROTO_CMD_CLEAR         = 0x33u,

    PROTO_CMD_ACK           = 0x7Eu,
    PROTO_CMD_NAK           = 0x7Fu
} proto_cmd_t;

/* ==================== 错误码 ==================== */

typedef enum {
    PROTO_ERR_NONE      = 0x00u,
    PROTO_ERR_CRC       = 0x01u,
    PROTO_ERR_LEN       = 0x02u,
    PROTO_ERR_CMD       = 0x03u,
    PROTO_ERR_PARAM     = 0x04u,
    PROTO_ERR_STATE     = 0x05u,
    PROTO_ERR_TIMEOUT   = 0x06u,
    PROTO_ERR_OVERFLOW  = 0x07u
} proto_err_t;

/* ==================== 接口 ==================== */

/**
 * @brief 计算 CRC-16/CCITT-FALSE。
 * @param data 数据
 * @param len  长度
 * @retval 16 位校验值
 */
uint16_t proto_crc16(const uint8_t *data, size_t len);

/**
 * @brief 构造一帧。
 * @param out     输出缓冲
 * @param out_cap 缓冲容量
 * @param cmd     命令码
 * @param seq     序号
 * @param payload 负载（可为 NULL）
 * @param len     负载长度
 * @retval 整帧字节数；参数非法或缓冲不足返回 0
 */
size_t proto_build(uint8_t *out, size_t out_cap,
                   uint8_t cmd, uint8_t seq,
                   const uint8_t *payload, size_t len);

/**
 * @brief 便捷函数：构造一个 IMAGE_BEGIN 帧。
 * @param out   输出缓冲
 * @param cap   缓冲容量
 * @param seq   序号
 * @param w,h   图像尺寸
 * @param x,y   屏幕位置
 * @retval 整帧字节数
 */
size_t proto_build_image_begin(uint8_t *out, size_t cap, uint8_t seq,
                               uint16_t w, uint16_t h, uint16_t x, uint16_t y);

/**
 * @brief 便捷函数：构造一个单字节参数的帧。
 * @details 用于 SET_SCALE / SET_BRIGHT。
 */
size_t proto_build_u8(uint8_t *out, size_t cap, uint8_t cmd, uint8_t seq, uint8_t value);

/**
 * @brief 便捷函数：构造一个双字节参数的帧。
 * @details 用于 CLEAR（颜色）、SET_POS 的一部分。
 */
size_t proto_build_u16(uint8_t *out, size_t cap, uint8_t cmd, uint8_t seq, uint16_t value);

#endif /* PROTO_H */
