/*
 * process_image.c - 用真实照片跑生产 YUV 算法链 (host 仿真, 效果可视化)
 *
 * 用法: process_image <in.bmp(24bit)> <out_prefix>
 * 输出: <prefix>_orig/fair/warm/cool/bw/vintage[_sticker].bmp
 *
 * 链路与真机 photo_worker 完全一致 (同一份 hw2d 源码):
 *   RGB888(BMP) -> YUYV422 (BT.601 全范围, 对内色度均值)
 *   -> hw2d_yuv_blur_y(磨皮40) -> hw2d_yuv_build_luts/yuv_filter(滤镜)
 *   -> hw2d_yuv_blend_circle(贴纸, 固定位) -> 恒等表 yuv_filter_rgb565
 *   -> RGB888 写回 BMP (RGB565 显示边界量化如实保留)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw2d.h"

#define W 320
#define H 240

static int read_bmp24(const char *path, uint8_t *rgb /* W*H*3, 行序自上而下 */)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(f);
        return -2;
    }
    int32_t w, h;
    uint16_t bpp;
    uint32_t off;
    memcpy(&off, hdr + 10, 4);
    memcpy(&w, hdr + 18, 4);
    memcpy(&h, hdr + 22, 4);
    memcpy(&bpp, hdr + 28, 2);
    if (w != W || abs(h) != H || bpp != 24) {
        fprintf(stderr, "expect %dx%d 24bit, got %dx%d %ubit\n", W, H, w, h, bpp);
        fclose(f);
        return -3;
    }
    int flip = h > 0;                       /* 正高度 = 自下而上存储 */
    uint32_t stride = ((W * 3 + 3) / 4) * 4;
    uint8_t *row = malloc(stride);
    for (int y = 0; y < H; y++) {
        int dy = flip ? (H - 1 - y) : y;
        fseek(f, (long)(off + (uint32_t)y * stride), SEEK_SET);
        if (fread(row, 1, stride, f) != stride) { free(row); fclose(f); return -4; }
        for (int x = 0; x < W; x++) {       /* BMP 为 BGR */
            rgb[(dy * W + x) * 3 + 0] = row[x * 3 + 2];
            rgb[(dy * W + x) * 3 + 1] = row[x * 3 + 1];
            rgb[(dy * W + x) * 3 + 2] = row[x * 3 + 0];
        }
    }
    free(row);
    fclose(f);
    return 0;
}

static int write_bmp24(const char *path, const uint8_t *rgb)
{
    uint32_t stride = ((W * 3 + 3) / 4) * 4, data = stride * H;
    uint8_t hdr[54] = { 0 };
    uint32_t u32;
    uint16_t u16;
    hdr[0] = 'B'; hdr[1] = 'M';
    u32 = 54 + data; memcpy(hdr + 2, &u32, 4);
    u32 = 54;        memcpy(hdr + 10, &u32, 4);
    u32 = 40;        memcpy(hdr + 14, &u32, 4);
    int32_t iw = W, ih = H;
    memcpy(hdr + 18, &iw, 4);
    memcpy(hdr + 22, &ih, 4);
    u16 = 1;  memcpy(hdr + 26, &u16, 2);
    u16 = 24; memcpy(hdr + 28, &u16, 2);
    u32 = data; memcpy(hdr + 34, &u32, 4);

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(hdr, 1, 54, f);
    uint8_t *row = malloc(stride);
    memset(row, 0, stride);
    for (int y = H - 1; y >= 0; y--) {
        for (int x = 0; x < W; x++) {
            row[x * 3 + 0] = rgb[(y * W + x) * 3 + 2];
            row[x * 3 + 1] = rgb[(y * W + x) * 3 + 1];
            row[x * 3 + 2] = rgb[(y * W + x) * 3 + 0];
        }
        fwrite(row, 1, stride, f);
    }
    free(row);
    fclose(f);
    return 0;
}

/* RGB888 -> YUYV422 (BT.601 全范围, 对内色度均值) */
static void rgb_to_yuyv(const uint8_t *rgb, uint8_t *yuyv)
{
    for (int i = 0; i < W * H; i += 2) {
        int r0 = rgb[i * 3], g0 = rgb[i * 3 + 1], b0 = rgb[i * 3 + 2];
        int r1 = rgb[(i + 1) * 3], g1 = rgb[(i + 1) * 3 + 1], b1 = rgb[(i + 1) * 3 + 2];
        int y0 = (77 * r0 + 150 * g0 + 29 * b0) >> 8;
        int y1 = (77 * r1 + 150 * g1 + 29 * b1) >> 8;
        int cb0 = 128 + (((b0 - y0) * 144) >> 8);
        int cb1 = 128 + (((b1 - y1) * 144) >> 8);
        int cr0 = 128 + (((r0 - y0) * 183) >> 8);
        int cr1 = 128 + (((r1 - y1) * 183) >> 8);
        yuyv[i * 2]     = (uint8_t)y0;
        yuyv[i * 2 + 1] = (uint8_t)((cb0 + cb1) / 2);
        yuyv[i * 2 + 2] = (uint8_t)y1;
        yuyv[i * 2 + 3] = (uint8_t)((cr0 + cr1) / 2);
    }
}

static void rgb565_to_rgb888(const uint16_t *px, uint8_t *rgb)
{
    for (int i = 0; i < W * H; i++) {
        uint16_t p = px[i];
        uint8_t r5 = (p >> 11) & 31, g6 = (p >> 5) & 63, b5 = p & 31;
        rgb[i * 3]     = (uint8_t)((r5 << 3) | (r5 >> 2));
        rgb[i * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
        rgb[i * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
    }
}

/* CCD 机型链: main/ccd_assets/ccd_<id>.l3d + 颗粒/暗角 (与 worker 同参数) */
static const struct { const char *id; int grain, hl, vig; } k_ccd[] = {
    { "f100", 12, 99, 30 }, { "z30", 14, 25, 16 },
    { "a620", 13, 25, 14 }, { "m532", 12, 30, 20 },
};

static int run_ccd(const uint8_t *src_yuyv, const char *prefix)
{
    static uint8_t yuyv[W * H * 2], lut[25 * 25 * 25 * 3];
    static uint16_t px[W * H];
    static uint8_t rgb[W * H * 3];
    hw2d_yuv_luts_t idl;
    memset(&idl, 0, sizeof(idl));
    hw2d_yuv_build_luts(hw2d_filter_get(HW2D_FILTER_ORIGINAL), &idl);
    for (unsigned c = 0; c < sizeof(k_ccd) / sizeof(k_ccd[0]); c++) {
        char path[256];
        snprintf(path, sizeof(path), "main/ccd_assets/ccd_%s.l3d", k_ccd[c].id);
        FILE *f = fopen(path, "rb");
        if (!f || fread(lut, 1, sizeof(lut), f) != sizeof(lut)) {
            fprintf(stderr, "lut %s missing\n", path);
            if (f) fclose(f);
            return 1;
        }
        fclose(f);
        memcpy(yuyv, src_yuyv, sizeof(yuyv));
        hw2d_yuv_blur_y(yuyv, yuyv, W, H, 40);
        hw2d_yuv_3dlut(yuyv, W * H, lut, 25);
        hw2d_yuv_grain(yuyv, W, H, (uint8_t)k_ccd[c].grain,
                       (uint8_t)k_ccd[c].hl, 42);
        hw2d_yuv_vignette(yuyv, W, H, (uint8_t)k_ccd[c].vig);
        hw2d_yuv_filter_rgb565(px, yuyv, W * H, &idl);
        rgb565_to_rgb888(px, rgb);
        snprintf(path, sizeof(path), "%s_ccd_%s.bmp", prefix, k_ccd[c].id);
        if (write_bmp24(path, rgb) != 0) return 1;
        printf("wrote %s\n", path);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.bmp out_prefix [sticker|ccd]\n", argv[0]);
        return 2;
    }
    int sticker = (argc > 3 && strcmp(argv[3], "sticker") == 0);
    int ccd = (argc > 3 && strcmp(argv[3], "ccd") == 0);
    static uint8_t rgb[W * H * 3], yuyv[W * H * 2], src_yuyv[W * H * 2];
    static uint16_t rgb565[W * H];
    if (read_bmp24(argv[1], rgb) != 0) {
        fprintf(stderr, "read %s failed\n", argv[1]);
        return 1;
    }
    rgb_to_yuyv(rgb, src_yuyv);
    if (ccd) return run_ccd(src_yuyv, argv[2]);

    static const char *const names[] = { "orig", "fair", "warm",
                                         "cool", "bw", "vintage" };
    hw2d_yuv_luts_t luts, idl;
    memset(&idl, 0, sizeof(idl));
    hw2d_yuv_build_luts(hw2d_filter_get(HW2D_FILTER_ORIGINAL), &idl);

    for (int fi = 0; fi < HW2D_FILTER_MAX; fi++) {
        memcpy(yuyv, src_yuyv, sizeof(yuyv));
        /* 与 photo_worker 同链: 磨皮40 -> 滤镜 -> 贴纸(可选) */
        hw2d_yuv_blur_y(yuyv, yuyv, W, H, 40);
        memset(&luts, 0, sizeof(luts));
        hw2d_yuv_build_luts(hw2d_filter_get((hw2d_filter_id_t)fi), &luts);
        hw2d_yuv_filter(yuyv, yuyv, W * H, &luts);
        if (sticker) {
            hw2d_yuv_blend_circle(yuyv, W, H, 60 + 200 / 4, 40 - 12, 21, 170);
            hw2d_yuv_blend_circle(yuyv, W, H, 60 + 150, 40 - 12, 21, 170);
            hw2d_yuv_blend_circle(yuyv, W, H, 160, 40 - 20, 14, 200);
        }
        hw2d_yuv_filter_rgb565(rgb565, yuyv, W * H, &idl);
        rgb565_to_rgb888(rgb565, rgb);
        char path[256];
        snprintf(path, sizeof(path), "%s_%s%s.bmp", argv[2], names[fi],
                 sticker ? "_sticker" : "");
        if (write_bmp24(path, rgb) != 0) {
            fprintf(stderr, "write %s failed\n", path);
            return 1;
        }
        printf("wrote %s\n", path);
    }
    return 0;
}
