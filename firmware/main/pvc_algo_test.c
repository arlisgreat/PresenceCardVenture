/*
 * pvc_algo_test.c - QEMU 仿真下的预览/拍照算法链自测 (-D PVC_ALGO_TEST)
 *
 * 在 qemu-system-xtensa -M esp32s3 上运行 (无相机/LCD/WiFi 外设):
 * 跳过 BSP/联网, 用合成帧跑真实 Xtensa 代码的完整算法链, 重点覆盖
 * host 测不到的部分 —— esp_new_jpeg 编码 (预编译 Xtensa 库) 与
 * esp_jpeg 解码回环, 以及 IDF 堆完整性 (HEAP_POISONING_COMPREHENSIVE)。
 *
 * 链路 (等价 photo_worker + preview 单帧):
 *   合成帧 -> filter_lut(预览) -> blur3x3 -> filter_exact
 *   -> pvc_jpeg_encode(q90/q60) -> pvc_jpeg_dims 校验
 *   -> jpg2rgb565 解码 -> swap16 -> 与编码源逐像素容差比对
 *   每轮 heap_caps_check_integrity_all + 栈水位输出。
 *
 * 结束标记 (runner 依据): "[ALGO_TEST] ALL PASS" / "[ALGO_TEST] FAILED n=?"
 * 内存: 优先 PSRAM; QEMU 无 PSRAM 时回落内部堆并降尺寸 160x120。
 */
#ifdef PVC_ALGO_TEST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "hw2d.h"
#include "pvc_jpeg.h"
#include "img_converters.h"

#define ITERS 12

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; \
    printf("[ALGO] FAIL %d: ", __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

static void *balloc(size_t sz, bool *psram)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) { *psram = true; return p; }
    *psram = false;
    return heap_caps_malloc(sz, MALLOC_CAP_8BIT);
}

static void synth_frame(uint16_t *p, uint32_t w, uint32_t h, unsigned seed)
{
    /* 平滑渐变 + 微噪声: JPEG 4:2:0 色度二次采样下随机色度必然大损耗,
     * 合成图必须以低频内容为主, 噪声仅作 ±3 扰动 (贴近真实画面统计) */
    uint32_t rnd = seed * 2654435761u + 1;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            rnd = rnd * 1664525u + 1013904223u;
            int n = (int)(rnd >> 30) - 1;                 /* -1..2 */
            int r = (int)((x * 255) / w) + n;
            int g = (int)((y * 255) / h) + n;
            int b = (int)(((x + y) * 255) / (w + h)) + n;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            p[y * w + x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) |
                                      (b >> 3));
        }
    }
}

void pvc_algo_test_run(void)
{
    printf("[ALGO_TEST] start (target=%s)\n", CONFIG_IDF_TARGET);

    uint32_t w = 320, h = 240;
    bool ps1, ps2, ps3, ps4, ps5;
    size_t npix = w * h;
    uint16_t *frame  = balloc(npix * 2, &ps1);
    uint16_t *work   = balloc(npix * 2, &ps2);
    uint16_t *snap   = balloc(npix * 2, &ps3);
    uint8_t  *enc    = balloc(128 * 1024, &ps4);
    uint8_t  *dec    = balloc(npix * 2, &ps5);
    if (!frame || !work || !snap || !enc || !dec) {
        /* QEMU 无 PSRAM 且内部堆不够: 降到 160x120 重试 */
        printf("[ALGO] QVGA alloc failed, fallback 160x120\n");
        w = 160; h = 120; npix = w * h;
        frame = frame ? frame : balloc(npix * 2, &ps1);
        work  = work  ? work  : balloc(npix * 2, &ps2);
        snap  = snap  ? snap  : balloc(npix * 2, &ps3);
        enc   = enc   ? enc   : balloc(64 * 1024, &ps4);
        dec   = dec   ? dec   : balloc(npix * 2, &ps5);
    }
    if (!frame || !work || !snap || !enc || !dec) {
        printf("[ALGO_TEST] FAILED n=alloc\n");
        return;
    }
    printf("[ALGO] buffers %lux%lu psram=%d%d%d%d%d\n",
           (unsigned long)w, (unsigned long)h, ps1, ps2, ps3, ps4, ps5);
    hw2d_init();

    /* 字节序/通道序哨兵: 纯红图回环后必须仍以红为主
     * (解码输出序或 RGB888 通道序若有错位, 红会变成蓝/绿, 立即暴露) */
    {
        for (size_t i = 0; i < npix; i++) frame[i] = 0xF800;  /* 纯红 */
        size_t jl = pvc_jpeg_encode(frame, w, h, 90, enc, 128 * 1024);
        CHECK(jl > 0, "sentinel encode failed");
        if (jl && jpg2rgb565(enc, jl, dec, JPG_SCALE_NONE)) {
            uint64_t rs = 0, bs = 0;
            const uint16_t *d16 = (const uint16_t *)dec;
            for (size_t i = 0; i < npix; i++) {
                uint16_t px = d16[i];      /* jpg2rgb565 输出小端, 直读 */
                rs += (px >> 11) & 31;
                bs += px & 31;
            }
            CHECK(rs / npix >= 24 && bs / npix <= 6,
                  "byte/channel order wrong: mean r=%u b=%u",
                  (unsigned)(rs / npix), (unsigned)(bs / npix));
        }
    }

    for (int it = 0; it < ITERS; it++) {
        synth_frame(frame, w, h, 100 + it);
        hw2d_filter_t f = *hw2d_filter_get(
            (hw2d_filter_id_t)(it % HW2D_FILTER_MAX));

        /* 预览路径 */
        hw2d_apply_filter_lut(work, frame, npix, &f);

        /* 拍照路径 */
        hw2d_blur3x3(work, frame, w, h, (uint8_t)(it * 17 % 101));
        hw2d_apply_filter_exact(snap, work, npix, &f);

        uint8_t q = (it & 1) ? 90 : 60;
        size_t jlen = pvc_jpeg_encode(snap, w, h, q, enc, 128 * 1024);
        CHECK(jlen > 0 && jlen < npix, "encode it=%d len=%u", it, (unsigned)jlen);
        if (!jlen) continue;

        uint32_t jw = 0, jh = 0;
        CHECK(pvc_jpeg_dims(enc, jlen, &jw, &jh) && jw == w && jh == h,
              "dims it=%d got %lux%lu", it, (unsigned long)jw, (unsigned long)jh);

        /* 解码回环 (jpg2rgb565 输出小端 RGB565, QEMU 哨兵实证) */
        if (jpg2rgb565(enc, jlen, dec, JPG_SCALE_NONE)) {
            uint64_t err_sum = 0;
            const uint16_t *d16 = (const uint16_t *)dec;
            for (size_t i = 0; i < npix; i++) {
                uint16_t px = d16[i];
                int dr = abs(((px >> 11) & 31) - ((snap[i] >> 11) & 31));
                int dg = abs(((px >> 5) & 63) - ((snap[i] >> 5) & 63));
                int db = abs((px & 31) - (snap[i] & 31));
                err_sum += (uint64_t)(dr + dg / 2 + db);
            }
            unsigned mean_x10 = (unsigned)(err_sum * 10 / npix);
            CHECK(mean_x10 < 40, "roundtrip it=%d q=%d mean_err=%u.%u",
                  it, q, mean_x10 / 10, mean_x10 % 10);
            printf("[ALGO] it=%d q=%d jpeg=%uB mean_err=%u.%u\n",
                   it, q, (unsigned)jlen, mean_x10 / 10, mean_x10 % 10);
        } else {
            CHECK(0, "decode it=%d failed", it);
        }

        /* 堆完整性 (HEAP_POISONING_COMPREHENSIVE 下等价越界检查) */
        CHECK(heap_caps_check_integrity_all(true), "heap corrupt it=%d", it);
    }

    /* ================= YUV422 全链路段 (生产主路径) ================= */
    {
        uint8_t *yuyv = (uint8_t *)frame;      /* 复用缓冲 (2B/px 同尺寸) */
        /* YUV 纯红哨兵: Y=76 Cb=84 Cr=255 -> 解码后应红占优 */
        for (size_t i = 0; i < npix * 2; i += 4) {
            yuyv[i] = 76; yuyv[i + 1] = 84; yuyv[i + 2] = 76; yuyv[i + 3] = 255;
        }
        size_t jl = pvc_jpeg_encode_yuv422(yuyv, w, h, 90, enc, 128 * 1024);
        CHECK(jl > 0, "yuv sentinel encode failed");
        if (jl && jpg2rgb565(enc, jl, dec, JPG_SCALE_NONE)) {
            uint64_t rs = 0, bs = 0;
            const uint16_t *d16 = (const uint16_t *)dec;
            for (size_t i = 0; i < npix; i++) {
                rs += (d16[i] >> 11) & 31;
                bs += d16[i] & 31;
            }
            CHECK(rs / npix >= 22 && bs / npix <= 8,
                  "yuv order wrong: mean r=%u b=%u",
                  (unsigned)(rs / npix), (unsigned)(bs / npix));
        }

        /* 滤镜+磨皮+贴纸 -> 422 编码 -> 解码 -> 与 yuv_filter_rgb565 参考比对 */
        static hw2d_yuv_luts_t luts, idl;
        for (int it = 0; it < 6; it++) {
            for (uint32_t y = 0; y < h; y++) {
                uint8_t *row = yuyv + (size_t)y * w * 2;
                for (uint32_t x = 0; x < w; x += 2) {
                    uint8_t Y0 = (uint8_t)((x * 255) / w);
                    uint8_t cb = (uint8_t)(64 + (y * 128) / h);
                    uint8_t cr = (uint8_t)(192 - (y * 128) / h);
                    row[x * 2] = Y0; row[x * 2 + 1] = cb;
                    row[x * 2 + 2] = Y0; row[x * 2 + 3] = cr;
                }
            }
            const hw2d_filter_t *f = hw2d_filter_get((hw2d_filter_id_t)it);
            hw2d_yuv_blur_y(yuyv, yuyv, w, h, 50);
            hw2d_yuv_build_luts(f, &luts);
            hw2d_yuv_filter(yuyv, yuyv, npix, &luts);
            hw2d_yuv_blend_circle(yuyv, w, h, (int)w / 2, (int)h / 3, 20, 170);

            /* 参考: 已滤镜 YUYV 恒等表转 RGB565 */
            hw2d_yuv_build_luts(hw2d_filter_get(HW2D_FILTER_ORIGINAL), &idl);
            hw2d_yuv_filter_rgb565((uint16_t *)work, yuyv, npix, &idl);

            size_t jn = pvc_jpeg_encode_yuv422(yuyv, w, h, 90, enc, 128 * 1024);
            CHECK(jn > 0, "yuv encode it=%d failed", it);
            uint32_t jw = 0, jh = 0;
            CHECK(jn && pvc_jpeg_dims(enc, jn, &jw, &jh) && jw == w && jh == h,
                  "yuv dims it=%d", it);
            if (jn && jpg2rgb565(enc, jn, dec, JPG_SCALE_NONE)) {
                uint64_t err_sum = 0;
                const uint16_t *pa = (const uint16_t *)work;
                const uint16_t *pb = (const uint16_t *)dec;
                for (size_t i = 0; i < npix; i++) {
                    int dr = abs(((pa[i] >> 11) & 31) - ((pb[i] >> 11) & 31));
                    int dg = abs(((pa[i] >> 5) & 63) - ((pb[i] >> 5) & 63));
                    int db = abs((pa[i] & 31) - (pb[i] & 31));
                    err_sum += (uint64_t)(dr + dg / 2 + db);
                }
                unsigned m10 = (unsigned)(err_sum * 10 / npix);
                CHECK(m10 < 40, "yuv roundtrip it=%d mean_err=%u.%u",
                      it, m10 / 10, m10 % 10);
                printf("[ALGO] yuv it=%d jpeg=%uB mean_err=%u.%u\n",
                       it, (unsigned)jn, m10 / 10, m10 % 10);
            } else if (jn) {
                CHECK(0, "yuv decode it=%d failed", it);
            }
            CHECK(heap_caps_check_integrity_all(true), "yuv heap it=%d", it);
        }
    }

    /* ================= 浸泡段: 编码器 open/close x200 泄漏检测 ================= */
    {
        size_t heap0 = esp_get_free_heap_size();
        for (int it = 0; it < 200; it++) {
            uint8_t *yuyv = (uint8_t *)frame;
            for (size_t i = 0; i < npix * 2; i += 4) {
                yuyv[i] = (uint8_t)(it + (i >> 3)); yuyv[i + 1] = 120;
                yuyv[i + 2] = (uint8_t)(it + (i >> 3)); yuyv[i + 3] = 136;
            }
            size_t jn = pvc_jpeg_encode_yuv422(yuyv, w, h, 60, enc, 128 * 1024);
            if (!jn || !jpg2rgb565(enc, jn, dec, JPG_SCALE_NONE)) {
                CHECK(0, "soak encode/decode it=%d", it);
                break;
            }
            if ((it % 50) == 49) {
                printf("[ALGO] soak it=%d heap=%u\n", it,
                       (unsigned)esp_get_free_heap_size());
            }
        }
        size_t heap1 = esp_get_free_heap_size();
        long drift = (long)heap0 - (long)heap1;
        CHECK(drift < 2048, "soak heap leak: drift=%ld bytes", drift);
        printf("[ALGO] soak done heap_drift=%ld\n", drift);
        CHECK(heap_caps_check_integrity_all(true), "soak heap corrupt");
    }

    printf("[ALGO] min_heap=%u stack_hw=%u\n",
           (unsigned)esp_get_minimum_free_heap_size(),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (g_fail) {
        printf("[ALGO_TEST] FAILED n=%d\n", g_fail);
    } else {
        printf("[ALGO_TEST] ALL PASS iters=%d %lux%lu\n", ITERS,
               (unsigned long)w, (unsigned long)h);
    }
}

#endif /* PVC_ALGO_TEST */
