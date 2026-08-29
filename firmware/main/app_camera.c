/*
 * app_camera.c - CoreS3 GC0308 DVP 摄像头驱动 (esp32-camera 组件)
 *
 * 引脚映射 (M5Stack CoreS3 官方 camera_config_t):
 *   SCCB: SDA=12 SCL=11 | D0..D7 = 39,40,41,42,15,16,48,47
 *   VSYNC=46 HREF=38 PCLK=45 | XCLK/PWDN/RESET 未接 (-1, 用 GC0308 内部时钟)
 *
 * 采集路径: GC0308 -> I2S 并行输入(DMA) -> PSRAM 双缓冲
 * 零拷贝: 预览/拍照直接使用 fb->buf, 渲染完毕 release 即可。
 */
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_camera.h"
#include "bsp/m5stack_core_s3.h"   /* BSP_I2C_NUM: SCCB 复用 BSP I2C 总线 */

#include "app_camera.h"

static const char *TAG = "camera";

static camera_fb_t *s_fb = NULL;   /* 当前持有的帧 (grab 后 release 前) */
static bool s_ready = false;       /* 未初始化/已关闭时 grab 快速返回, 防日志刷屏 */

esp_err_t app_camera_init(void)
{
    if (s_ready) return ESP_OK;    /* 静默唤醒后补初始化, 允许重复调用 */
    camera_config_t camera_config = {
        .pin_pwdn      = -1,
        .pin_reset     = -1,
        .pin_xclk      = -1,
        /* SCCB 与 PMIC/触摸共用 BSP 的 I2C1 (11/12): 引脚必须 NC +
         * sccb_i2c_port 复用已装驱动, 自建总线会 acquire fail 且把触摸搞挂
         * (真机实测: i2c.common acquire bus failed -> 触摸无响应) */
        .pin_sscb_sda  = -1,
        .pin_sscb_scl  = -1,
        .pin_d7        = 47,
        .pin_d6        = 48,
        .pin_d5        = 16,
        .pin_d4        = 15,
        .pin_d3        = 42,
        .pin_d2        = 41,
        .pin_d1        = 40,
        .pin_d0        = 39,
        .pin_vsync     = 46,
        .pin_href      = 38,
        .pin_pclk      = 45,
        .xclk_freq_hz  = 20000000,
        .ledc_timer    = LEDC_TIMER_0,
        .ledc_channel  = LEDC_CHANNEL_0,
        /* 全链路 YUV 架构: 传感器原生 YCbYCr 直出 (寄存器序 Y Cb Y Cr =
         * esp_new_jpeg 编码器格式), 拍照零色彩空间往返; RGB565 仅显示边界 */
        .pixel_format  = PIXFORMAT_YUV422,
        /* 系统统一 320x240: 采集/预览/拍照零缩放 */
        .frame_size    = FRAMESIZE_QVGA,
        .jpeg_quality  = 0,
        .fb_count      = 2,                /* 双缓冲, PSRAM */
        .fb_location   = CAMERA_FB_IN_PSRAM,
        .grab_mode     = CAMERA_GRAB_LATEST, /* 预览始终显示最新帧 */
        .sccb_i2c_port = BSP_I2C_NUM,      /* =1, bsp_display_start 已装驱动 */
    };

    esp_err_t err = esp_camera_init(&camera_config);
    if (err == ESP_OK) {
        s_ready = true;
        sensor_t *s = esp_camera_sensor_get();
        if (s) {
            /* 画质默认值微调: 亮度 +1, 对照度 +1 (GC0308 默认偏暗) */
            s->set_brightness(s, 1);
            s->set_contrast(s, 1);
            /* 装配方向: 屏已转 180 度, 传感器同步转 (vflip+hmirror,
             * 寄存器级, 预览与拍照存片同源同向, 零 CPU) */
            s->set_vflip(s, 1);
            s->set_hmirror(s, 1);
            ESP_LOGI(TAG, "GC0308 ready: QVGA YUV422 (frame %u)", s->status.framesize);
        }
    } else {
        ESP_LOGE(TAG, "esp_camera_init failed: %s (0x%x), 请检查摄像头排线",
                 esp_err_to_name(err), err);
    }
    return err;
}

const app_camera_frame_t *app_camera_grab(void)
{
    static app_camera_frame_t f;
    if (!s_ready) return NULL;
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGW(TAG, "fb_get timeout");
        return NULL;
    }
    if (s_fb) esp_camera_fb_return(s_fb);   /* 归还上一帧, 保持双缓冲轮转 */
    s_fb = fb;
    f.buf    = (const uint16_t *)fb->buf;
    f.width  = fb->width;
    f.height = fb->height;
    f.size   = fb->len;
    return &f;
}

void app_camera_release(void)
{
    if (s_fb) {
        esp_camera_fb_return(s_fb);
        s_fb = NULL;
    }
}

void app_camera_shutdown(void)
{
    if (!s_ready) return;
    s_ready = false;
    app_camera_release();
    esp_camera_deinit();
}
