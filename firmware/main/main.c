/*
 * main.c - M5Stack CoreS3 Presence Card 固件入口
 *
 * 启动流程:
 *   1. bsp_display_start()        初始化 ILI9342C LCD + LVGL (esp_lvgl_port)
 *   2. bsp_sdcard_mount()         挂载 SD 卡 (相册存照 + 断电待传队列; 失败降级)
 *   3. ui_beauty_camera_create()  创建相机 UI (含 25FPS 预览定时器)
 *   4. app_camera_init()          初始化 GC0308 (esp32-camera, DVP+I2S DMA)
 *   5. pvc_net_start()            联网层: WiFi -> 配对(§1) -> 幂等上传队列(§2)
 *
 * 帧管线: UI 定时器 app_camera_grab() 拿最新帧 -> hw2d 定点滤镜/贴纸 -> canvas,
 *         渲染结束 app_camera_release() 归还缓冲 (PSRAM 双缓冲, 零拷贝)。
 * 权威规范: PresenceCardVenture/docs/02-device-api-v1.md
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include "bsp/m5stack_core_s3.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "app_camera.h"
#include "ui_beauty_camera.h"
#include "pvc_net.h"
#include "pvc_config.h"
#include "pvc_power.h"
#include "pvc_clock.h"

static const char *TAG = "main";

/* 联网状态 -> 状态栏短文案 */
static void net_status_cb(pvc_net_state_t st, const char *detail)
{
    (void)detail;
    static const char *const txt[] = {
        [PVC_NET_IDLE] = "--",
        [PVC_NET_PROVISIONING] = "BLE",
        [PVC_NET_WIFI_CONNECTING] = "WiFi..",
        [PVC_NET_PAIRING] = "Pair",
        [PVC_NET_ONLINE] = "Online",
        [PVC_NET_OFFLINE] = "Off",
    };
    ui_net_set_status(txt[st]);
}

void app_main(void)
{
    /* 定时器唤醒 = 静默轮询 (§6): 不亮屏不开相机, 同步完成即回睡 */
    bool quiet = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
    printf("[FW] boot presence-card fw=%s api_base=%s quiet=%d\n",
           FW_VERSION, PVC_API_BASE, (int)quiet);

    /* 1. 初始化 LCD + LVGL。覆盖 BSP 默认:
     *    - LVGL 任务绑 core0 (默认 -1 不绑核, 会与 core1 的拍照 worker 抢核)
     *    - 栈 7168->10240 (相册 JPEG 解码 + FATFS I/O 跑在此任务栈上) */
    bsp_display_cfg_t disp_cfg = {
        .lvgl_port_cfg = {
            .task_priority = 4,
            .task_stack = 10240,
            .task_affinity = 0,
            .task_max_sleep_ms = 500,
            .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
            .timer_period_ms = 5,
        },
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        },
    };
    lv_display_t *disp = bsp_display_start_with_config(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return;
    }
    if (!quiet) bsp_display_backlight_on();
    ESP_LOGI(TAG, "CoreS3 display + LVGL ready");

    /* 计时: TZ + 冷启动从 BM8563 恢复 (I2C 已由 BSP 初始化) */
    pvc_clock_init();

    /* 2. SD 卡: 相册 /sdcard/DCIM + 待传队列 /sdcard/queue。
     *    挂载失败不致命: 相册不可用, 待传队列降级 PSRAM。 */
    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sdcard mount failed (%s): album off, RAM upload queue",
                 esp_err_to_name(err));
    }

    /* 3. 创建相机 UI (需在 LVGL 初始化后) */
    ui_beauty_camera_create();

    /* 4. 初始化 GC0308: VGA RGB565, PSRAM 双缓冲 (esp32-camera DVP)。
     *    静默轮询不开相机 (省电), 用户触摸时由 pvc_power 补开。 */
    if (!quiet) {
        err = app_camera_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "camera init failed: %s (0x%x), 请检查 esp32-camera 组件与摄像头排线",
                     esp_err_to_name(err), err);
        }
        ESP_LOGI(TAG, "camera started");
    }

    /* 5. 联网层 (docs/02 §1/§2/§6): WiFi -> 配对 -> 上传闭环 */
    static const pvc_net_ui_t net_ui = {
        .show_pair_code = ui_net_show_pair,
        .hide_pair_code = ui_net_hide_pair,
        .show_prov      = ui_net_show_prov,
        .status         = net_status_cb,
        .feed_update    = ui_net_feed_updated,
    };
    pvc_config_set_apply_cb(ui_apply_remote_config);
    err = pvc_net_start(&net_ui);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pvc_net_start failed: %s", esp_err_to_name(err));
    }

    /* 6. 省电管理 (§6): 60s 无操作入睡, 5 分钟定时静默轮询, 触摸唤醒 */
    pvc_power_init(quiet);
}
