#include "pvc_jpeg.h"

#include "esp_log.h"
#include "esp_jpeg_enc.h"

static const char *TAG = "pvc_jpeg";

size_t pvc_jpeg_encode(const uint16_t *rgb565, uint32_t w, uint32_t h,
                       uint8_t quality, uint8_t *out, size_t out_cap)
{
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = (int)w;
    cfg.height = (int)h;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
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
    jpeg_error_t ret = jpeg_enc_process(h_enc, (const uint8_t *)rgb565,
                                        (int)(w * h * 2), out, (int)out_cap,
                                        &out_len);
    jpeg_enc_close(h_enc);
    if (ret != JPEG_ERR_OK || out_len <= 0) {
        ESP_LOGE(TAG, "enc process failed ret=%d", (int)ret);
        return 0;
    }
    return (size_t)out_len;
}
