#include "pvc_jpeg.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"

static const char *TAG = "pvc_jpeg";

/*
 * esp_new_jpeg 编码器不支持任何 RGB565 输入 (LE/BE 均仅解码方向支持,
 * QEMU 仿真实测 + esp_jpeg_common.h 注释证实)。编码前展开为 RGB888。
 * 转换缓冲内部常驻复用 (QVGA 225KB, PSRAM); 本接口非线程安全 ——
 * 仅限单一调用方 (photo_worker / algo test) 串行使用。
 */
static uint8_t *s_rgb888;
static size_t   s_rgb888_cap;

static bool ensure_rgb888(size_t need)
{
    if (s_rgb888_cap >= need) return true;
    if (s_rgb888) heap_caps_free(s_rgb888);
    s_rgb888 = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rgb888) s_rgb888 = heap_caps_malloc(need, MALLOC_CAP_8BIT);
    s_rgb888_cap = s_rgb888 ? need : 0;
    return s_rgb888 != NULL;
}

size_t pvc_jpeg_encode(const uint16_t *rgb565, uint32_t w, uint32_t h,
                       uint8_t quality, uint8_t *out, size_t out_cap)
{
    size_t npix = (size_t)w * h;
    if (!ensure_rgb888(npix * 3)) {
        ESP_LOGE(TAG, "rgb888 buffer alloc failed");
        return 0;
    }
    /* RGB565(LE) -> RGB888 (低位起 r,g,b; 低位复制扩展) */
    uint8_t *o = s_rgb888;
    for (size_t i = 0; i < npix; i++) {
        uint16_t p = rgb565[i];
        uint8_t r5 = (p >> 11) & 0x1F, g6 = (p >> 5) & 0x3F, b5 = p & 0x1F;
        *o++ = (uint8_t)((r5 << 3) | (r5 >> 2));
        *o++ = (uint8_t)((g6 << 2) | (g6 >> 4));
        *o++ = (uint8_t)((b5 << 3) | (b5 >> 2));
    }

    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = (int)w;
    cfg.height = (int)h;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB888;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = quality;
    cfg.task_enable = false;           /* 同步编码, 无内部任务 */

    jpeg_enc_handle_t h_enc = NULL;
    if (jpeg_enc_open(&cfg, &h_enc) != JPEG_ERR_OK || !h_enc) {
        ESP_LOGE(TAG, "enc open failed (%lux%lu q%d)",
                 (unsigned long)w, (unsigned long)h, quality);
        return 0;
    }
    int out_len = 0;
    jpeg_error_t ret = jpeg_enc_process(h_enc, s_rgb888, (int)(npix * 3),
                                        out, (int)out_cap, &out_len);
    jpeg_enc_close(h_enc);
    if (ret != JPEG_ERR_OK || out_len <= 0) {
        ESP_LOGE(TAG, "enc process failed ret=%d", (int)ret);
        return 0;
    }
    return (size_t)out_len;
}

size_t pvc_jpeg_encode_yuv422(const uint8_t *yuyv, uint32_t w, uint32_t h,
                              uint8_t quality, uint8_t *out, size_t out_cap)
{
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = (int)w;
    cfg.height = (int)h;
    cfg.src_type = JPEG_PIXEL_FORMAT_YCbYCr;   /* 传感器字节流直通 */
    /* 422 与源精确匹配 (保真要求: 420 会垂直减半色度); q90 QVGA ~40-60KB */
    cfg.subsampling = JPEG_SUBSAMPLE_422;
    cfg.quality = quality;
    cfg.task_enable = false;

    jpeg_enc_handle_t h_enc = NULL;
    if (jpeg_enc_open(&cfg, &h_enc) != JPEG_ERR_OK || !h_enc) {
        ESP_LOGE(TAG, "yuv enc open failed (%lux%lu q%d)",
                 (unsigned long)w, (unsigned long)h, quality);
        return 0;
    }
    int out_len = 0;
    jpeg_error_t ret = jpeg_enc_process(h_enc, yuyv, (int)(w * h * 2),
                                        out, (int)out_cap, &out_len);
    jpeg_enc_close(h_enc);
    if (ret != JPEG_ERR_OK || out_len <= 0) {
        ESP_LOGE(TAG, "yuv enc process failed ret=%d", (int)ret);
        return 0;
    }
    return (size_t)out_len;
}
