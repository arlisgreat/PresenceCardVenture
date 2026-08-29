/*
 * test_threads.c - hw2d 并发契约压测 (配合 TSAN, -fsanitize=thread)
 *
 * 模拟真机并发拓扑:
 *   线程A(=core0 预览): 自持 luts, yuv_filter_rgb565 + swap16 + fill
 *   线程B(=core1 worker): 自持 luts, yuv_filter(原地) + yuv_blend_circle
 *                          + blur (blur 平面契约: 仅 worker 使用)
 * 各 2000 轮, TSAN 验证 "LUT 上下文调用方持有" 设计无隐藏共享可变态。
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw2d.h"

#define W 320
#define H 240
#define ROUNDS 2000

static int g_fail;

static void *preview_thread(void *arg)
{
    (void)arg;
    static uint8_t yuyv[W * H * 2];
    static uint16_t rgb[W * H];
    static uint16_t line[W];
    hw2d_yuv_luts_t luts;
    memset(&luts, 0, sizeof(luts));
    for (int i = 0; i < ROUNDS; i++) {
        hw2d_yuv_build_luts(hw2d_filter_get((hw2d_filter_id_t)(i % HW2D_FILTER_MAX)),
                            &luts);
        hw2d_yuv_filter_rgb565(rgb, yuyv, W * H, &luts);
        hw2d_fill(line, W, (uint16_t)i);
        hw2d_swap16((uint16_t *)yuyv, (uint16_t *)yuyv, W);  /* 原地小段 */
    }
    return NULL;
}

static void *worker_thread(void *arg)
{
    (void)arg;
    static uint8_t yuyv[W * H * 2];
    hw2d_yuv_luts_t luts;
    memset(&luts, 0, sizeof(luts));
    for (int i = 0; i < ROUNDS; i++) {
        hw2d_yuv_blur_y(yuyv, yuyv, W, H, (uint8_t)(i % 101));  /* blur 契约持有者 */
        hw2d_yuv_build_luts(hw2d_filter_get(
            (hw2d_filter_id_t)((i + 3) % HW2D_FILTER_MAX)), &luts);
        hw2d_yuv_filter(yuyv, yuyv, W * H, &luts);
        hw2d_yuv_blend_circle(yuyv, W, H, 100 + (i % 100), 100, 20, 170);
    }
    return NULL;
}

int main(void)
{
    hw2d_init();
    pthread_t a, b;
    pthread_create(&a, NULL, preview_thread, NULL);
    pthread_create(&b, NULL, worker_thread, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    if (g_fail) { printf("thread tests: FAILED\n"); return 1; }
    printf("thread tests: ALL PASS (%d rounds x2)\n", ROUNDS);
    return 0;
}
