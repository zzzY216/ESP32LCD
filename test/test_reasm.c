/**
 * @file    test_reasm.c
 * @brief   验证 ESP32 侧的「跨 BLE 包帧重组」逻辑。
 *
 * @details 这是针对真实故障的回归测试。
 *
 *   【故障现象】
 *   手机能通过 BLE 连上 ESP32、能发数据，但 STM32 完全不动。
 *   日志显示：
 *       frame passthrough: cmd=0x11 seq=138 len=509   <- 少了 11 字节
 *       RX 11 bytes: EE 8B EF 33 ...                  <- 缺少的尾巴
 *       not a frame (head=74 AD), treated as raw data <- 碎片被误处理
 *
 *   【根因】
 *   BLE 单包载荷上限 = MTU-3 = 509 字节。
 *   一个 IMAGE_DATA 帧 = 6 + 512 + 2 = 520 字节 > 509。
 *   GATT 栈自动拆成 509 + 11 两包。ESP32 却把每包当独立帧处理，
 *   碎片的帧头认不出来，被当成"裸图像数据"重新封装 → 数据流错乱。
 *
 *   【验证目标】
 *   重组器必须能把 509+11 拼回完整的 520 字节帧，
 *   且必须能处理：连续多帧、帧内嵌 A5 5A、垃圾字节、半帧。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PROTO_SYNC0       0xA5u
#define PROTO_SYNC1       0x5Au
#define PROTO_MAX_PAYLOAD 1024u
#define PROTO_HEADER_SIZE 6u
#define PROTO_TAIL_SIZE   2u
#define PROTO_MAX_FRAME   (PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD + PROTO_TAIL_SIZE)

static int g_run = 0, g_fail = 0;

#define CHECK(cond, msg, ...)                                          \
    do {                                                               \
        g_run++;                                                       \
        if (!(cond)) { g_fail++; printf("  [FAIL] " msg "\n", ##__VA_ARGS__); } \
        else { printf("  [ OK ] " msg "\n", ##__VA_ARGS__); }          \
    } while (0)

/* ---------- 被测逻辑（与 main.c 的 feed_accum 保持一致） ---------- */

static uint8_t  s_accum[PROTO_MAX_FRAME];
static size_t   s_accum_len = 0;

/* 记录每次透传出去的帧 */
static int      g_frame_count = 0;
static uint16_t g_frame_len[64];
static uint8_t  g_frame_cmd[64];

static void process_frame(const uint8_t *frame, size_t total)
{
    if (g_frame_count < 64) {
        g_frame_len[g_frame_count] = (uint16_t)total;
        g_frame_cmd[g_frame_count] = frame[2];
    }
    g_frame_count++;
}

static bool feed_accum(const uint8_t *data, size_t len)
{
    size_t   total, need, consumed;
    uint16_t payload_len;
    bool     saw_frame = false;

    if (data == NULL || len == 0) return false;

    if (s_accum_len + len > sizeof(s_accum)) {
        s_accum_len = 0;
        if (len > sizeof(s_accum)) len = sizeof(s_accum);
    }
    memcpy(s_accum + s_accum_len, data, len);
    s_accum_len += len;

    consumed = 0;
    for (;;) {
        size_t   avail = s_accum_len - consumed;
        uint8_t *p     = s_accum + consumed;

        if (avail < 2) break;

        if (p[0] != PROTO_SYNC0 || p[1] != PROTO_SYNC1) {
            size_t skip = 1;
            while (skip + 1 < avail &&
                   !(p[skip] == PROTO_SYNC0 && p[skip + 1] == PROTO_SYNC1)) {
                skip++;
            }
            if (consumed == 0 && !saw_frame) {
                if (s_accum_len >= len) s_accum_len -= len;
                else s_accum_len = 0;
                return false;
            }
            consumed += skip;
            continue;
        }

        if (avail < PROTO_HEADER_SIZE) { saw_frame = true; break; }

        payload_len = (uint16_t)(p[4] | ((uint16_t)p[5] << 8));

        if (payload_len > PROTO_MAX_PAYLOAD) { consumed += 1; continue; }

        total = PROTO_HEADER_SIZE + (size_t)payload_len + PROTO_TAIL_SIZE;

        if (avail < total) { saw_frame = true; break; }

        saw_frame = true;
        process_frame(p, total);
        consumed += total;
    }

    if (consumed > 0) {
        if (consumed >= s_accum_len) s_accum_len = 0;
        else {
            need = s_accum_len - consumed;
            memmove(s_accum, s_accum + consumed, need);
            s_accum_len = need;
        }
    }
    return saw_frame;
}

/* ---------- 辅助：造一个帧 ---------- */

static size_t make_frame(uint8_t *out, uint8_t cmd, uint8_t seq,
                         uint16_t payload_len, uint8_t fill)
{
    size_t i;
    out[0] = PROTO_SYNC0;
    out[1] = PROTO_SYNC1;
    out[2] = cmd;
    out[3] = seq;
    out[4] = (uint8_t)(payload_len & 0xFF);
    out[5] = (uint8_t)(payload_len >> 8);
    for (i = 0; i < payload_len; i++) out[PROTO_HEADER_SIZE + i] = fill;
    out[PROTO_HEADER_SIZE + payload_len] = 0x12;
    out[PROTO_HEADER_SIZE + payload_len + 1] = 0x34;
    return PROTO_HEADER_SIZE + payload_len + PROTO_TAIL_SIZE;
}

static void reset_all(void)
{
    s_accum_len = 0;
    g_frame_count = 0;
}

/* ---------- 测试 ---------- */

int main(void)
{
    uint8_t frame[1200];
    size_t  flen;

    printf("=== 跨 BLE 包帧重组验证 ===\n\n");

    /* 1. 核心场景：520 字节帧被拆成 509 + 11 */
    printf("[核心：509+11 拆包重组]\n");
    {
        reset_all();
        flen = make_frame(frame, 0x11, 138, 512, 0xAA);
        CHECK(flen == 520, "帧总长 520 字节（实际 %zu）", flen);

        /* 第 1 包：509 字节 —— 不完整，不应透传 */
        feed_accum(frame, 509);
        CHECK(g_frame_count == 0, "只收到 509 字节时不透传（等剩余）");

        /* 第 2 包：剩余 11 字节 —— 应触发完整帧 */
        feed_accum(frame + 509, 11);
        CHECK(g_frame_count == 1, "补齐后透传 1 帧（实际 %d）", g_frame_count);
        CHECK(g_frame_len[0] == 520, "透传长度 520（实际 %u）", g_frame_len[0]);
        CHECK(g_frame_cmd[0] == 0x11, "命令码 0x11（实际 0x%02X）", g_frame_cmd[0]);
    }

    /* 2. 一包正好一个完整帧 */
    printf("\n[单包完整帧]\n");
    {
        reset_all();
        flen = make_frame(frame, 0x01, 0, 0, 0);
        CHECK(flen == 8, "PING 帧 8 字节");
        feed_accum(frame, flen);
        CHECK(g_frame_count == 1 && g_frame_len[0] == 8, "直接透传");
    }

    /* 3. 连续多帧（模拟手机连续发送） */
    printf("\n[连续 3 帧]\n");
    {
        reset_all();
        for (int k = 0; k < 3; k++) {
            flen = make_frame(frame, 0x11, (uint8_t)k, 512, (uint8_t)k);
            /* 每帧都按 509 + 11 拆 */
            feed_accum(frame, 509);
            feed_accum(frame + 509, 11);
        }
        CHECK(g_frame_count == 3, "透传 3 帧（实际 %d）", g_frame_count);
        CHECK(g_frame_len[2] == 520, "第 3 帧 520 字节");
    }

    /* 4. 多个小帧挤在一个 BLE 包里 */
    printf("\n[一包内多个小帧]\n");
    {
        reset_all();
        size_t off = 0;
        off += make_frame(frame + off, 0x01, 0, 0, 0);    /* 8 */
        off += make_frame(frame + off, 0x20, 1, 0, 0);    /* 8 */
        off += make_frame(frame + off, 0x21, 2, 0, 0);    /* 8 */
        CHECK(off == 24, "3 个 PING 类帧共 24 字节");

        feed_accum(frame, off);
        CHECK(g_frame_count == 3, "一次解析出 3 帧（实际 %d）", g_frame_count);
    }

    /* 5. 帧内嵌 A5 5A 不能误判为帧头 */
    printf("\n[负载内含 A5 5A]\n");
    {
        reset_all();
        flen = make_frame(frame, 0x11, 5, 512, 0xAA);
        /* 在负载中部植入 A5 5A，模拟像素数据恰好等于同步字 */
        frame[100] = 0xA5;
        frame[101] = 0x5A;
        frame[200] = 0xA5;
        frame[201] = 0x5A;

        feed_accum(frame, 509);
        feed_accum(frame + 509, 11);
        CHECK(g_frame_count == 1, "仍只解析出 1 帧（未误判）");
        CHECK(g_frame_len[0] == 520, "长度仍为 520");
    }

    /* 6. 垃圾字节在前，然后是真帧 */
    printf("\n[前导垃圾字节]\n");
    {
        reset_all();
        flen = make_frame(frame, 0x11, 7, 512, 0xBB);
        /* 前面加 5 个垃圾字节 */
        feed_accum((const uint8_t *)"\x01\x02\x03\x04\x05", 5);
        /* 垃圾不算帧，返回 false，实际中会走裸数据兜底 */
        feed_accum(frame, 509);
        feed_accum(frame + 509, 11);
        CHECK(g_frame_count == 1, "跳过垃圾后解析出 1 帧（实际 %d）", g_frame_count);
    }

    /* 7. 半帧卡住后接着来新帧 */
    printf("\n[半帧后接新帧]\n");
    {
        reset_all();
        flen = make_frame(frame, 0x11, 8, 512, 0xCC);
        feed_accum(frame, 300);              /* 只发 300 字节，卡住 */
        CHECK(g_frame_count == 0, "半帧不透传");

        /* 补齐剩余 */
        feed_accum(frame + 300, 220);
        CHECK(g_frame_count == 1, "补齐后透传 1 帧");
    }

    /* 8. 完整帧 + 下一帧的前半部分 */
    printf("\n[帧边界不对齐]\n");
    {
        reset_all();
        uint8_t big[1200];
        size_t  l1 = make_frame(big, 0x11, 9, 512, 0xDD);
        size_t  l2 = make_frame(big + l1, 0x11, 10, 512, 0xEE);

        /* 一次发 520 + 200 = 720 字节 */
        feed_accum(big, l1 + 200);
        CHECK(g_frame_count == 1, "解析出第 1 个完整帧");

        /* 补齐第 2 帧 */
        feed_accum(big + l1 + 200, l2 - 200);
        CHECK(g_frame_count == 2, "解析出第 2 个完整帧（实际 %d）", g_frame_count);
    }

    /* 9. 非帧数据应返回 false（走裸数据兜底） */
    printf("\n[非帧数据]\n");
    {
        reset_all();
        bool r = feed_accum((const uint8_t *)"hello world", 11);
        CHECK(r == false, "普通文本返回 false");
        CHECK(s_accum_len == 0, "缓冲被清空（撤销追加）");
    }

    /* 10. 长度字段异常时的重同步 */
    printf("\n[LEN 异常重同步]\n");
    {
        reset_all();
        uint8_t bad[16];
        /* 假帧头 + 超大 LEN */
        bad[0] = 0xA5; bad[1] = 0x5A;
        bad[2] = 0x11; bad[3] = 0x00;
        bad[4] = 0xFF; bad[5] = 0xFF;   /* LEN = 65535 > MAX_PAYLOAD */
        feed_accum(bad, 6);
        CHECK(g_frame_count == 0, "非法 LEN 不透传");
    }

    printf("\n=== %d 项检查，%d 项失败 ===\n", g_run, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
