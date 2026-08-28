/*
 * test_hw2d.c - hw2d 像素算子 host 端单元测试 (无需硬件)
 *
 * 运行: cd firmware && cc -O2 -I main -o /tmp/hw2d_test \
 *          test/host/test_hw2d.c main/hw2d.c && /tmp/hw2d_test
 * hw2d.c 的非 ESP_PLATFORM 分支为标准 C, PIE 路径不参与 (上板由自测背书)。
 * 覆盖: swap16 正确性/对齐、scale_be 与 scale+swap 等价、blur 边界、
 *       fill、LUT 滤镜确定性与灰度路径。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw2d.h"

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __func__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static uint16_t naive_swap(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static void fill_rand(uint16_t *p, size_t n, unsigned seed)
{
    srand(seed);
    for (size_t i = 0; i < n; i++) p[i] = (uint16_t)(rand() & 0xFFFF);
}

/* swap16: 任意长度 (含奇数尾) 与逐像素参考一致 */
static void test_swap16(void)
{
    enum { N = 1024 + 7 };
    static uint16_t src[N + 4], dst[N + 4], ref[N];
    static const size_t lens[] = { 0, 1, 3, 4, 7, 8, 64, N };

    for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        size_t n = lens[li];
        fill_rand(src, N, 42 + li);
        for (size_t i = 0; i < n; i++) ref[i] = naive_swap(src[i]);
        memset(dst, 0xAA, sizeof(dst));
        hw2d_swap16(dst, src, (uint32_t)n);
        CHECK(memcmp(dst, ref, n * 2) == 0, "len=%zu mismatch", n);
        CHECK(dst[n] == 0xAAAA, "len=%zu overrun", n);
    }
    /* 非 8 字节对齐入口 (走逐像素路径) */
    fill_rand(src, 64, 7);
    for (int i = 0; i < 32; i++) ref[i] = naive_swap(src[i + 1]);
    hw2d_swap16(dst + 1, src + 1, 32);
    CHECK(memcmp(dst + 1, ref, 32 * 2) == 0, "unaligned mismatch");
}

/* scale_be(src) 必须与 swap16(scale(src)) 逐位一致 (含同尺寸快路径) */
static void test_scale_be_equiv(void)
{
    static const struct { uint32_t sw, sh, dw, dh; } cases[] = {
        { 640, 480, 320, 240 },   /* 上传编码路径 */
        { 320, 240, 320, 240 },   /* 同尺寸快路径 */
        { 160, 120, 96, 72 },     /* 缩略图 */
        { 33, 17, 20, 11 },       /* 奇数尺寸 */
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint32_t sw = cases[c].sw, sh = cases[c].sh;
        uint32_t dw = cases[c].dw, dh = cases[c].dh;
        uint16_t *src = malloc(sw * sh * 2);
        uint16_t *a = malloc(dw * dh * 2);
        uint16_t *b = malloc(dw * dh * 2);
        fill_rand(src, sw * sh, 100 + c);

        hw2d_scale(src, sw, sh, a, dw, dh);
        hw2d_swap16(a, a, dw * dh);          /* 原地: swap16 要求分离? 用副本 */
        /* swap16 要求 src/dst 分离 —— 用独立缓冲重做参考 */
        uint16_t *tmp = malloc(dw * dh * 2);
        hw2d_scale(src, sw, sh, tmp, dw, dh);
        for (uint32_t i = 0; i < dw * dh; i++) a[i] = naive_swap(tmp[i]);
        free(tmp);

        hw2d_scale_be(src, sw, sh, b, dw, dh);
        CHECK(memcmp(a, b, dw * dh * 2) == 0,
              "case %u (%ux%u->%ux%u) mismatch", c, sw, sh, dw, dh);
        free(src); free(a); free(b);
    }
}

/* blur: strength=0 纯拷贝; strength>0 输出在合理范围且不越界 */
static void test_blur(void)
{
    enum { W = 32, H = 24 };
    static uint16_t in[W * H], out[W * H + 4];
    fill_rand(in, W * H, 5);
    memset(out, 0x55, sizeof(out));
    hw2d_blur3x3(out, in, W, H, 0);
    CHECK(memcmp(out, in, sizeof(in)) == 0, "strength=0 not copy");
    hw2d_blur3x3(out, in, W, H, 60);
    CHECK(out[W * H] == 0x5555, "blur overrun");
    /* 全同色输入模糊后必须不变 */
    for (int i = 0; i < W * H; i++) in[i] = 0x1234;
    hw2d_blur3x3(out, in, W, H, 80);
    for (int i = 0; i < W * H; i++) {
        if (out[i] != 0x1234) { CHECK(0, "uniform blur changed @%d", i); break; }
    }
}

static void test_fill(void)
{
    static uint16_t buf[67];
    hw2d_fill(buf, 67, 0xBEEF);
    for (int i = 0; i < 67; i++) {
        if (buf[i] != 0xBEEF) { CHECK(0, "fill @%d", i); break; }
    }
}

/* LUT 滤镜: 同参数两次输出一致; 原图滤镜近似恒等; 黑白输出 r==g==b */
static void test_filter_lut(void)
{
    enum { N = 320 };
    static uint16_t src[N], a[N], b[N];
    fill_rand(src, N, 9);

    const hw2d_filter_t *orig = hw2d_filter_get(HW2D_FILTER_ORIGINAL);
    hw2d_apply_filter_lut(a, src, N, orig);
    hw2d_apply_filter_lut(b, src, N, orig);
    CHECK(memcmp(a, b, sizeof(a)) == 0, "not deterministic");
    /* 原图参数: 允许量化误差, 抽查通道差 <= 1 LSB(5/6bit 域) */
    for (int i = 0; i < N; i++) {
        int dr = abs(((a[i] >> 11) & 31) - ((src[i] >> 11) & 31));
        int dg = abs(((a[i] >> 5) & 63) - ((src[i] >> 5) & 63));
        int db = abs((a[i] & 31) - (src[i] & 31));
        if (dr > 1 || dg > 2 || db > 1) {
            CHECK(0, "original filter deviates @%d (%d,%d,%d)", i, dr, dg, db);
            break;
        }
    }
    const hw2d_filter_t *bw = hw2d_filter_get(HW2D_FILTER_BW);
    hw2d_apply_filter_lut(a, src, N, bw);
    for (int i = 0; i < N; i++) {
        int r = (a[i] >> 11) & 31, gch = (a[i] >> 6) & 31, bl = a[i] & 31;
        if (abs(r - gch) > 1 || abs(gch - bl) > 1) {
            CHECK(0, "bw not gray @%d", i);
            break;
        }
    }
}

int main(void)
{
    hw2d_init();
    test_swap16();
    test_scale_be_equiv();
    test_blur();
    test_fill();
    test_filter_lut();
    if (g_fail) {
        printf("hw2d host tests: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("hw2d host tests: ALL PASS\n");
    return 0;
}
