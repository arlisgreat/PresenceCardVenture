/*
 * pvc_jpeg.h - JPEG 编码薄封装 (espressif/esp_new_jpeg)
 *
 * 乐鑫针对 ESP32-S3 优化的编码器, 较通用 jpge 快 2-4x;
 * 支持 RGB565 小端直入 —— 编码路径不再需要字节交换。
 * 线程约定: 可在任意任务调用 (每次调用独立 open/close, 无共享状态)。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RGB565(LE) -> JPEG。out 由调用方提供 (建议容量 >= w*h/2, VGA q90 实测 ~100KB)。
 * 返回实际字节数; 失败返回 0。quality 1-100。
 */
size_t pvc_jpeg_encode(const uint16_t *rgb565, uint32_t w, uint32_t h,
                       uint8_t quality, uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
