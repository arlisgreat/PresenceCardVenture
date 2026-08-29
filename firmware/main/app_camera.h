/*
 * app_camera.h - CoreS3 GC0308 摄像头封装 (基于 espressif/esp32-camera, DVP 并行接口)
 *
 * 硬件事实: CoreS3 的 GC0308 是 DVP 8bit 并行 + SCCB(I2C) 控制,
 * 并不支持 esp_video/V4L2 (该组件仅适配 ESP32-P4 的 MIPI-CSI)。
 * 本封装使用 esp32-camera 的 I2S DMA 采集, 双缓冲落于 PSRAM:
 *
 *   GC0308 --DVP--> I2S 并行输入 --DMA--> PSRAM fb[0]/fb[1]
 *   UI 定时器: app_camera_grab() -> 直接读最新帧 (零拷贝) -> 渲染 -> release
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一帧 RGB565 数据 (缓冲在 PSRAM, 由 esp_camera DMA 填充) */
typedef struct {
    const uint16_t *buf;   /* RGB565 帧数据 */
    uint32_t width;        /* 320 */
    uint32_t height;       /* 240 */
    uint32_t size;         /* 字节数 */
} app_camera_frame_t;

/**
 * @brief 初始化 GC0308 (QVGA 320x240 RGB565, 双缓冲 PSRAM, LATEST 抓帧)
 * @note  必须在 LVGL / UI 创建完成后调用
 */
esp_err_t app_camera_init(void);

/**
 * @brief 抓取最新一帧 (阻塞至 DMA 完成一帧; 可安全地与上一帧重叠)
 * @return 帧指针, 在下一次 grab/release 前有效; 失败返回 NULL
 */
const app_camera_frame_t *app_camera_grab(void);

/* 归还当前帧缓冲 (抓到的帧用完必须归还, 否则采集会阻塞) */
void app_camera_release(void);

/* 关闭摄像头 */
void app_camera_shutdown(void);

#ifdef __cplusplus
}
#endif
