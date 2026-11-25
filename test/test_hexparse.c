/**
 * @file    test_hexparse.c
 * @brief   验证 ASCII 十六进制文本解析逻辑。
 * @details 关键要验证两件事：
 *            1. 「A5 5A 01 00」这类文本能被正确转成字节
 *            2. 真正的二进制帧（含 0xA5、0x5A 等非 ASCII 字节）
 *               不会被误判成文本，否则会导致数据损坏
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

static int g_run = 0, g_fail = 0;

#define CHECK(cond, msg, ...)                                      \
    do {                                                           \
        g_run++;                                                   \
        if (!(cond)) { g_fail++; printf("  [FAIL] " msg "\n", ##__VA_ARGS__); } \
        else { printf("  [ OK ] " msg "\n", ##__VA_ARGS__); }      \
    } while (0)

/* ---------- 从 main.c 复制过来的实现（保持一致） ---------- */

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

static int parse_hex_text(const uint8_t *in, size_t in_len,
                          uint8_t *out, size_t out_cap)
{
    size_t i = 0, n = 0;
    int    hi = -1;

    if (in == NULL || out == NULL || in_len == 0) return 0;

    {
        size_t hex_count = 0, bad_chars = 0;
        for (i = 0; i < in_len; i++) {
            uint8_t c = in[i];
            if (is_hex_char(c)) hex_count++;
            else if (c == ' ' || c == ',' || c == '\r' || c == '\n' ||
                     c == '\t' || c == ':' || c == '-') { }
            else if (c == 'x' || c == 'X') { }
            else bad_chars++;
        }
        if (bad_chars > 0 || hex_count < 2) return 0;
    }

    for (i = 0; i < in_len; i++) {
        uint8_t c = in[i];
        int v;
        /* 跳过 0x / 0X 前缀 */
        if (c == '0' && (i + 1) < in_len && (in[i+1] == 'x' || in[i+1] == 'X')) {
            i++;
            continue;
        }
        if (!is_hex_char(c)) {
            continue;
        }
        v = hex_val(c);
        if (v < 0) return 0;
        if (hi < 0) hi = v;
        else {
            if (n >= out_cap) return (int)n;
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0) return 0;
    return (int)n;
}

/* ---------- 测试 ---------- */

int main(void)
{
    uint8_t out[512];
    int n;

    printf("=== ASCII 十六进制文本解析验证 ===\n\n");

    /* 1. 标准格式（用户实际发的） */
    printf("[标准格式]\n");
    {
        const char *s = "A5 5A 01 00 00 00 74 F2";
        n = parse_hex_text((const uint8_t*)s, strlen(s), out, sizeof(out));
        CHECK(n == 8, "解析出 8 字节（实际 %d）", n);
        if (n == 8) {
            const uint8_t want[8] = {0xA5,0x5A,0x01,0x00,0x00,0x00,0x74,0xF2};
            CHECK(memcmp(out, want, 8) == 0, "字节内容完全正确");
            CHECK(out[0] == 0xA5, "首字节 = 0xA5（不是 0x41）");
        }
    }

    /* 2. 无空格连写 */
    printf("\n[无分隔符]\n");
    {
        const char *s = "A55A010000007 4F2";
        n = parse_hex_text((const uint8_t*)s, strlen(s), out, sizeof(out));
        CHECK(n == 8 && out[0] == 0xA5, "连写也能解析（%d 字节）", n);
    }

    /* 3. 小写 */
    printf("\n[小写]\n");
    {
        const char *s = "a5 5a 01 00 00 00 74 f2";
        n = parse_hex_text((const uint8_t*)s, strlen(s), out, sizeof(out));
        CHECK(n == 8 && out[0] == 0xA5 && out[7] == 0xF2, "小写正常（%d 字节）", n);
    }

    /* 4. 0x 前缀 */
    printf("\n[0x 前缀]\n");
    {
        const char *s = "0xA5,0x5A,0x01,0x00";
        n = parse_hex_text((const uint8_t*)s, strlen(s), out, sizeof(out));
        CHECK(n == 4 && out[0] == 0xA5 && out[1] == 0x5A,
              "0x 前缀格式（%d 字节）", n);
    }

    /* 5. ★关键★ 真正的二进制帧不能被误判 */
    printf("\n[二进制数据不应被误判]\n");
    {
        const uint8_t binary[8] = {0xA5,0x5A,0x01,0x00,0x00,0x00,0x74,0xF2};
        n = parse_hex_text(binary, 8, out, sizeof(out));
        CHECK(n == 0, "真实二进制帧返回 0（不当作文本）");

        /* 再测一个含大量可打印字符但混有高字节的 */
        const uint8_t mixed[6] = {'A','B','C',0xFF,'D','E'};
        n = parse_hex_text(mixed, 6, out, sizeof(out));
        CHECK(n == 0, "含 0xFF 的数据返回 0");
    }

    /* 6. 纯字母文本（非 hex） */
    printf("\n[非法输入]\n");
    {
        const char *s = "hello world";
        n = parse_hex_text((const uint8_t*)s, strlen(s), out, sizeof(out));
        CHECK(n == 0, "普通文本返回 0");

        const char *s2 = "A";      /* 单个 hex 字符 */
        n = parse_hex_text((const uint8_t*)s2, strlen(s2), out, sizeof(out));
        CHECK(n == 0, "单字符返回 0");

        const char *s3 = "A5 5";   /* 奇数个 hex 字符 */
        n = parse_hex_text((const uint8_t*)s3, strlen(s3), out, sizeof(out));
        CHECK(n == 0, "奇数个字符返回 0");
    }

    /* 7. 短帧 */
    printf("\n[短帧]\n");
    {
        const char *s = "A5 5A";
        n = parse_hex_text((const uint8_t*)s, strlen(s), out, sizeof(out));
        CHECK(n == 2 && out[0] == 0xA5 && out[1] == 0x5A, "2 字节（%d）", n);
    }

    /* 8. 带换行/制表符 */
    printf("\n[含换行制表符]\n");
    {
        const char *s = "A5\t5A\r\n01 00";
        n = parse_hex_text((const uint8_t*)s, strlen(s), out, sizeof(out));
        CHECK(n == 4 && out[0] == 0xA5, "换行制表符被正确跳过（%d 字节）", n);
    }

    printf("\n=== %d 项检查，%d 项失败 ===\n", g_run, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
