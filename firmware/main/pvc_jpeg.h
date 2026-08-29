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
 * RGB565(LE) -> JPEG。out 由调用方提供 (建议容量 >= w*h/2)。
 * 返回实际字节数; 失败返回 0。quality 1-100。
 * esp_new_jpeg 编码器不收 RGB565 (QEMU 实测): 内部展开 RGB888 后编码,
 * 转换缓冲常驻复用。非线程安全 —— 仅限单一调用方串行使用 (photo_worker)。
 */
size_t pvc_jpeg_encode(const uint16_t *rgb565, uint32_t w, uint32_t h,
                       uint8_t quality, uint8_t *out, size_t out_cap);

/*
 * YUV422 (YCbYCr packed, 传感器原生序) -> JPEG 直通编码:
 * 零色彩空间往返, 编码器最优路径 (含旋转支持的唯一格式)。
 */
size_t pvc_jpeg_encode_yuv422(const uint8_t *yuyv, uint32_t w, uint32_t h,
                              uint8_t quality, uint8_t *out, size_t out_cap);

/* 快速解析 JPEG SOF 尺寸 (防异常尺寸图解码溢出); 解析失败返回 false */
bool pvc_jpeg_dims(const uint8_t *jpg, size_t len, uint32_t *w, uint32_t *h);

#ifdef __cplusplus
}
#endif
