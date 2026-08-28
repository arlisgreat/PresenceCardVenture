#include "pvc_power.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"     /* esp_get_free_heap_size */
#include "esp_timer.h"
#include "driver/gpio.h"

#include "bsp/m5stack_core_s3.h"
#include "lvgl.h"

#include "app_camera.h"
#include "pvc_trace.h"
#include "net/pvc_net.h"
#include "net/pvc_upload.h"

static const char *TAG = "pvc_power";

#define IDLE_SLEEP_MS    60000               /* §6: 无操作 60s 入睡 */
#define DRAIN_GRACE_MS   60000               /* 在线未同步完: 最多再等 60s */
#define SLEEP_PERIOD_US  (5ULL * 60 * 1000000)  /* §6: 5-15 分钟, 取 5 分钟 */
#define QUIET_MIN_MS     8000                /* 静默唤醒至少在线 8s 让联网任务起跑 */
#define QUIET_MAX_MS     45000               /* 静默唤醒硬上限 (目标 <20s, 兜底 45s) */

/* CoreS3 触摸 (FT6336U) INT 引脚 = GPIO21 (RTC IO, 可作 deep sleep 唤醒源)。
 * 若真机验证发现 INT 引脚不符, 用 build_flags -D PVC_TOUCH_INT_GPIO=xx 覆盖;
 * 设为 -1 则只保留定时唤醒。 */
#ifndef PVC_TOUCH_INT_GPIO
#define PVC_TOUCH_INT_GPIO 21
#endif

static bool s_quiet;

/* LVGL 距上次触摸/按键的毫秒数 (加锁读取, 任意任务安全)。
 * 返回 false = 没拿到显示锁, 本 tick 跳过判定 (宁可晚睡/晚醒也不误判)。 */
static bool get_inactive(uint32_t *out)
{
    if (!bsp_display_lock(100)) return false;
    *out = lv_display_get_inactive_time(NULL);
    bsp_display_unlock();
    return true;
}

static void enter_sleep(const char *reason)
{
    /* 联调日志: 全栈据此核对 "单次唤醒 <20s 在线" (§6) */
    printf("[FW] SLEEP reason=%s uptime_ms=%lu queue=%d synced=%d\n",
           reason, (unsigned long)(esp_timer_get_time() / 1000),
           pvc_upload_depth(), (int)pvc_net_synced());

    app_camera_shutdown();
    bsp_display_backlight_off();

    esp_sleep_enable_timer_wakeup(SLEEP_PERIOD_US);
#if PVC_TOUCH_INT_GPIO >= 0
    /* 触摸 INT 低有效; 控制器常供电, 断言即唤醒 */
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PVC_TOUCH_INT_GPIO, 0);
#endif
    esp_deep_sleep_start();
}

/* 静默轮询中用户触摸 -> 恢复正常交互 */
static void exit_quiet(void)
{
    ESP_LOGI(TAG, "touch during quiet poll, waking UI");
    s_quiet = false;
    app_camera_init();
    bsp_display_backlight_on();
}

static void power_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* 每 30s 心跳: 堆水位/队列深度, 供耐久测试做泄漏与阻塞检测 */
        if ((++tick % 30) == 0) {
            PVC_EV("stat heap=%lu min_heap=%lu queue=%d state=%d up_ms=%lu",
                   (unsigned long)esp_get_free_heap_size(),
                   (unsigned long)esp_get_minimum_free_heap_size(),
                   pvc_upload_depth(), (int)pvc_net_state(),
                   (unsigned long)(esp_timer_get_time() / 1000));
            /* 各任务栈剩余最小水位 (字节; IDF StackType_t=1B)。
             * analyzer C17 红线: <512 FAIL, <1024 WARN。-1 = 任务不存在 */
            static const char *const k_watch[] = {
                "pvc_net", "photo_wk", "taskLVGL", "cam_task", "sys_evt",
            };
            char line[112];
            int off = snprintf(line, sizeof(line), "stack self=%u",
                               (unsigned)uxTaskGetStackHighWaterMark(NULL));
            for (unsigned i = 0; i < sizeof(k_watch) / sizeof(k_watch[0]); i++) {
                TaskHandle_t h = xTaskGetHandle(k_watch[i]);
                off += snprintf(line + off, sizeof(line) - (size_t)off,
                                " %s=%d", k_watch[i],
                                h ? (int)uxTaskGetStackHighWaterMark(h) : -1);
                if (off >= (int)sizeof(line)) break;
            }
            PVC_EV("%s", line);
        }

        pvc_net_state_t st = pvc_net_state();
        /* 配网 / 配对中用户正在跟屏幕交互, 不休眠 */
        bool interactive = (st == PVC_NET_PROVISIONING || st == PVC_NET_PAIRING);

        uint32_t idle = 0;
        if (!get_inactive(&idle)) continue;
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000);

        if (s_quiet) {
            /* "曾被触摸" 不能用 idle<阈值 判 (开机时 idle≈up 从 0 涨起, 必误判);
             * 有输入事件时 LVGL 空闲计时归零, 只有 idle 明显小于开机时长才是真触摸 */
            bool touched = (idle + 3000 < up);
            if (interactive || touched) {
                exit_quiet();
                continue;
            }
            if ((up >= QUIET_MIN_MS && pvc_net_synced()) || up >= QUIET_MAX_MS) {
                enter_sleep(pvc_net_synced() ? "poll_done" : "poll_timeout");
            }
            continue;
        }

        if (idle < IDLE_SLEEP_MS || interactive) continue;
        /* 60s 无操作: 在线但还有待传/未拉取时给宽限, 干完再睡 */
        if (!pvc_net_synced() && st == PVC_NET_ONLINE &&
            idle < IDLE_SLEEP_MS + DRAIN_GRACE_MS) {
            continue;
        }
        enter_sleep("idle");
    }
}

void pvc_power_init(bool quiet_boot)
{
    s_quiet = quiet_boot;
    xTaskCreatePinnedToCore(power_task, "pvc_power", 4096, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "power manager started (quiet=%d, touch_int=%d)",
             (int)quiet_boot, PVC_TOUCH_INT_GPIO);
}
