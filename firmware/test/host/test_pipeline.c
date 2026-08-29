/*
 * test_pipeline.c - 预览/拍照算法链 host 仿真 (配合 ASAN/UBSAN)
 *
 * 运行 (见 run_asan.sh): clang -fsanitize=address,undefined 编译后执行。
 * 覆盖真实 QVGA 尺寸下的完整算法链多轮迭代:
 *   预览: ensure_lut/filter_lut(6 滤镜轮换) + blend(贴纸) + 缩略图 scale
 *   拍照: blur3x3 -> filter_exact -> (swap16/scale_be 编码前置路径)
 *   守护: 所有目标缓冲带前后哨兵红区, 每轮校验; SOF 解析器喂随机/截断输入
 * JPEG 编解码为 Xtensa 预编译库, 不在 host 覆盖 (由 QEMU 用例覆盖)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw2d.h"
#include "pvc_jpeg.h"

#define W 320
#define H 240
#define NPIX (W * H)
#define GUARD 16          /* 哨兵像素数 (ASAN 之外的双保险) */
#define ITERS 40

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; \
    printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

static uint16_t *galloc(size_t npix)
{
    uint16_t *p = malloc((npix + 2 * GUARD) * 2);
    for (int i = 0; i < GUARD; i++) p[i] = 0xA5A5;
    for (size_t i = npix + GUARD; i < npix + 2 * GUARD; i++) p[i] = 0xA5A5;
    return p + GUARD;
}

static void gcheck(uint16_t *p, size_t npix, const char *name)
{
    uint16_t *base = p - GUARD;
    for (int i = 0; i < GUARD; i++) {
        if (base[i] != 0xA5A5 || base[npix + GUARD + i] != 0xA5A5) {
            CHECK(0, "%s guard smashed @%d", name, i);
            return;
        }
    }
}

static void gfree(uint16_t *p) { free(p - GUARD); }

static void rand_frame(uint16_t *p, size_t n, unsigned seed)
{
    srand(seed);
    for (size_t i = 0; i < n; i++) p[i] = (uint16_t)rand();
}

int main(void)
{
    hw2d_init();

    uint16_t *frame  = galloc(NPIX);
    uint16_t *canvas = galloc(NPIX);
    uint16_t *work   = galloc(NPIX);
    uint16_t *snap   = galloc(NPIX);
    uint16_t *thumb  = galloc(22 * 22);
    uint16_t *be     = galloc(NPIX);
    uint16_t *line   = galloc(W);

    for (int it = 0; it < ITERS; it++) {
        rand_frame(frame, NPIX, 1000 + it);
        hw2d_filter_id_t fid = (hw2d_filter_id_t)(it % HW2D_FILTER_MAX);
        hw2d_filter_t f = *hw2d_filter_get(fid);
        f.bias = (int8_t)((it * 7) % 129 - 64);      /* 扫 bias 全范围 */

        /* ---- 预览链 ---- */
        hw2d_apply_filter_lut(canvas, frame, NPIX, &f);
        hw2d_fill(line, W, 0xFFFF);
        /* 贴纸混合: 含贴边极端坐标 (行内区间) */
        hw2d_alpha_blend(&canvas[10 * W + 0], &line[0], W, 170);
        hw2d_alpha_blend(&canvas[(H - 1) * W + 0], &line[0], W, 255);
        hw2d_alpha_blend(&canvas[20 * W + 5], &line[5], 1, 1);
        hw2d_scale(frame, W, H, thumb, 22, 22);

        /* ---- 拍照链 ---- */
        hw2d_blur3x3(work, frame, W, H, (uint8_t)(it * 13 % 101));
        hw2d_apply_filter_exact(snap, work, NPIX, &f);
        hw2d_swap16(be, snap, NPIX);                 /* 编码前置 (feed 方向同函数) */
        hw2d_scale_be(frame, W, H, be, W, H);        /* 同尺寸 BE 快路径 */
        hw2d_scale_be(frame, W, H, be, 160, 120);    /* 缩小 BE 路径 */

        gcheck(canvas, NPIX, "canvas");
        gcheck(work, NPIX, "work");
        gcheck(snap, NPIX, "snap");
        gcheck(thumb, 22 * 22, "thumb");
        gcheck(be, NPIX, "be");
        gcheck(line, W, "line");
    }

    /* ---- YUV422 算子 (生产主路径) ---- */
    {
        static uint8_t yuyv[NPIX * 2], ybak[NPIX * 2];
        static uint16_t rgbout[NPIX];
        srand(31337);
        for (size_t i = 0; i < sizeof(yuyv); i++) yuyv[i] = (uint8_t)rand();
        memcpy(ybak, yuyv, sizeof(yuyv));

        hw2d_yuv_luts_t idl, luts;
        memset(&idl, 0, sizeof(idl));
        hw2d_yuv_build_luts(hw2d_filter_get(HW2D_FILTER_ORIGINAL), &idl);
        /* 恒等表: 滤镜过后必须逐字节不变 */
        hw2d_yuv_filter(yuyv, yuyv, NPIX, &idl);
        CHECK(memcmp(yuyv, ybak, sizeof(yuyv)) == 0, "identity yuv filter changed data");

        /* 黑白 (sat=0): 输出色度必须恒 128 */
        memset(&luts, 0, sizeof(luts));
        hw2d_yuv_build_luts(hw2d_filter_get(HW2D_FILTER_BW), &luts);
        hw2d_yuv_filter(yuyv, ybak, NPIX, &luts);
        for (size_t i = 1; i < sizeof(yuyv); i += 2) {
            if (yuyv[i] != 128) { CHECK(0, "bw chroma !=128 @%zu", i); break; }
        }

        /* 恒色帧: Y 磨皮后必须不变 (含色度保留) */
        for (size_t i = 0; i < sizeof(yuyv); i += 4) {
            yuyv[i] = 90; yuyv[i+1] = 100; yuyv[i+2] = 90; yuyv[i+3] = 160;
        }
        memcpy(ybak, yuyv, sizeof(yuyv));
        hw2d_yuv_blur_y(yuyv, yuyv, W, H, 70);
        CHECK(memcmp(yuyv, ybak, sizeof(yuyv)) == 0, "uniform blur_y changed");

        /* blend_circle: 越界整圆裁剪 (不写内存); 合法圆提亮 Y */
        hw2d_yuv_blend_circle(yuyv, W, H, -5, 10, 20, 200);
        hw2d_yuv_blend_circle(yuyv, W, H, W - 1, H - 1, 8, 200);
        CHECK(memcmp(yuyv, ybak, sizeof(yuyv)) == 0, "oob circle wrote");
        hw2d_yuv_blend_circle(yuyv, W, H, W / 2, H / 2, 20, 255);
        CHECK(yuyv[((size_t)(H / 2) * W + W / 2) * 2] > 200, "blend no lighten");

        /* yuv->rgb565: 灰点 (128,128,128) 应转出近中灰 */
        for (size_t i = 0; i < sizeof(yuyv); i++) yuyv[i] = 128;
        hw2d_yuv_filter_rgb565(rgbout, yuyv, NPIX, &idl);
        int r = (rgbout[0] >> 11) & 31, g = (rgbout[0] >> 5) & 63, b = rgbout[0] & 31;
        CHECK(abs(r - 16) <= 1 && abs(g - 32) <= 2 && abs(b - 16) <= 1,
              "gray convert got %d,%d,%d", r, g, b);
    }

    /* ---- SOF 解析器: 随机与截断输入不越界、构造头正确解析 ---- */
    {
        uint8_t jbuf[512];
        uint32_t jw, jh;
#ifndef FUZZ_ITERS
#define FUZZ_ITERS 200
#endif
        for (int it = 0; it < FUZZ_ITERS; it++) {
            srand(9000 + it);
            size_t len = (size_t)(rand() % sizeof(jbuf));
            for (size_t i = 0; i < len; i++) jbuf[i] = (uint8_t)rand();
            pvc_jpeg_dims(jbuf, len, &jw, &jh);   /* 只要求不越界不挂 */
        }
        /* 最小合法 SOF0 头 */
        static const uint8_t sof[] = {
            0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00,
            0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0xF0, 0x01, 0x40,
        };
        CHECK(pvc_jpeg_dims(sof, sizeof(sof), &jw, &jh) &&
              jw == 320 && jh == 240, "sof parse got %ux%u", jw, jh);
        CHECK(!pvc_jpeg_dims(sof, 9, &jw, &jh), "truncated should fail");
    }

    gfree(frame); gfree(canvas); gfree(work); gfree(snap);
    gfree(thumb); gfree(be); gfree(line);

    if (g_fail) {
        printf("pipeline host tests: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("pipeline host tests: ALL PASS (%d iters)\n", ITERS);
    return 0;
}
