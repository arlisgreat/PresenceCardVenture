#include "pvc_net.h"
#include "pvc_store.h"
#include "pvc_pair.h"
#include "pvc_prov.h"
#include "pvc_upload.h"
#include "pvc_feed.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_heap_caps.h"

static const char *TAG = "pvc_net";

#define BIT_WIFI_UP   BIT0
#define BIT_PHOTO     BIT1
#define BIT_FEED      BIT2        /* UI 请求立即拉 feed / 发点赞 */
#define DRAIN_PERIOD_MS 60000     /* 空闲时每 60s 尝试排空一次待传队列 */
#define FEED_POLL_MS  (5 * 60 * 1000)  /* §3: 轮询间隔 >= 5 分钟 */

static EventGroupHandle_t s_ev;
static pvc_net_ui_t s_ui;
static pvc_net_state_t s_state = PVC_NET_IDLE;
static volatile bool s_feed_synced;   /* 本次启动后 feed 是否成功轮询过 */

static void set_state(pvc_net_state_t st, const char *detail)
{
    s_state = st;
    if (s_ui.status) s_ui.status(st, detail);
}

pvc_net_state_t pvc_net_state(void) { return s_state; }

/* ---------------- WiFi STA ---------------- */
static bool s_provisioning;    /* BLE 配网期间抑制断线状态刷新 */

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_provisioning) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, BIT_WIFI_UP);
        if (s_provisioning) return;    /* 配网握手期的断连由 prov manager 处理 */
        set_state(PVC_NET_OFFLINE, "wifi lost");
        /* 事件循环内禁止阻塞: 立即重连, 驱动层自带扫描间隔即天然节流 */
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_ev, BIT_WIFI_UP);
    }
}

/*
 * WiFi 启动。凭据优先级:
 *   1. NVS 已存 (BLE 配过网 / 上次 build-flag 写入) -> 直接 STA 连接
 *   2. 编译期 PVC_WIFI_SSID (开发后门) -> 写入并连接
 *   3. 都没有 -> BLE 配网 (阻塞至手机下发凭据, manager 自动连接)
 */
static esp_err_t wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_cb, NULL));

    if (pvc_prov_is_provisioned()) {
        /* 凭据在 NVS, esp_wifi 启动时自动加载 */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        return ESP_OK;
    }

    if (PVC_WIFI_SSID[0]) {
        wifi_config_t wc = { 0 };
        strncpy((char *)wc.sta.ssid, PVC_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, PVC_WIFI_PASS, sizeof(wc.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGW(TAG, "using build-flag wifi credentials (dev only)");
        return ESP_OK;
    }

    /* BLE 配网 */
    set_state(PVC_NET_PROVISIONING, NULL);
    s_provisioning = true;
    esp_err_t err = pvc_prov_run(&s_ui);
    s_provisioning = false;
    return err;
}

/* ---------------- 主状态机任务 ---------------- */
static void net_task(void *arg)
{
    (void)arg;
    set_state(PVC_NET_WIFI_CONNECTING, NULL);
    if (wifi_start() != ESP_OK) {
        set_state(PVC_NET_OFFLINE, "setup fail");
        vTaskDelete(NULL);
        return;
    }
    xEventGroupWaitBits(s_ev, BIT_WIFI_UP, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "wifi connected");

    /* SNTP 对时 (§0: 设备本地时间用 SNTP; 失败不致命) */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_cfg);

    pvc_upload_init();
    pvc_feed_init();

    int64_t last_feed_poll = 0;   /* us; 0 = 从未拉过, 上线即拉 */
    for (;;) {
        /* 无 token -> 配对 (§1) */
        while (!pvc_store_token()[0]) {
            xEventGroupWaitBits(s_ev, BIT_WIFI_UP, pdFALSE, pdTRUE, portMAX_DELAY);
            set_state(PVC_NET_PAIRING, NULL);
            if (pvc_pair_run(&s_ui) != ESP_OK) {
                set_state(PVC_NET_OFFLINE, "pair fail");
                vTaskDelay(pdMS_TO_TICKS(15000));
            }
        }
        set_state(PVC_NET_ONLINE, NULL);

        /* 排空待传队列; 401 -> 清 token 回配对流程 (§0 错误表) */
        xEventGroupWaitBits(s_ev, BIT_WIFI_UP, pdFALSE, pdTRUE, portMAX_DELAY);
        pvc_up_result_t r = pvc_upload_drain();
        if (r == PVC_UP_AUTH_FAIL) {
            ESP_LOGW(TAG, "401 TOKEN_INVALID: clearing token, back to pairing");
            pvc_store_clear_token();
            continue;
        }

        /* 发送积压点赞 (§3.4) */
        if (pvc_feed_process_reactions() == PVC_FEED_AUTH) {
            pvc_store_clear_token();
            continue;
        }

        /* feed 轮询 (§3.1/3.2): 首次上线 / UI 请求 / 每 5 分钟 */
        int64_t now = esp_timer_get_time();
        bool feed_req = (xEventGroupClearBits(s_ev, BIT_FEED) & BIT_FEED) != 0;
        if (feed_req || last_feed_poll == 0 ||
            now - last_feed_poll >= (int64_t)FEED_POLL_MS * 1000) {
            int fresh = pvc_feed_poll();
            if (fresh == PVC_FEED_AUTH) {
                pvc_store_clear_token();
                continue;
            }
            if (fresh >= 0) {
                last_feed_poll = now;
                s_feed_synced = true;
                if (s_ui.feed_update) {
                    pvc_feed_item_t tmp[PVC_FEED_MAX];
                    s_ui.feed_update(pvc_feed_snapshot(tmp, PVC_FEED_MAX), fresh);
                }
            }
            /* 拉取失败: 保持 last_feed_poll, 下轮唤醒再试 */
        }

        /* 等新照片/feed 信号或周期唤醒 */
        xEventGroupWaitBits(s_ev, BIT_PHOTO | BIT_FEED, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(r == PVC_UP_RETRY_LATER ? 15000
                                                                  : DRAIN_PERIOD_MS));
        xEventGroupClearBits(s_ev, BIT_PHOTO);
    }
}

void pvc_net_signal_feed(void)
{
    if (s_ev) xEventGroupSetBits(s_ev, BIT_FEED);
}

bool pvc_net_synced(void)
{
    return s_state == PVC_NET_ONLINE && s_feed_synced && pvc_upload_depth() == 0;
}

/* ---------------- 公共 API ---------------- */
esp_err_t pvc_net_start(const pvc_net_ui_t *ui)
{
    if (ui) s_ui = *ui;
    s_ev = xEventGroupCreate();
    if (!s_ev) return ESP_ERR_NO_MEM;

    esp_err_t err = pvc_store_init();
    if (err != ESP_OK) return err;

    /* TLS 握手约需 40KB 堆 (§6); 任务栈给足 8KB 供 http+json */
    BaseType_t ok = xTaskCreatePinnedToCore(net_task, "pvc_net", 8192, NULL,
                                            4, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t pvc_net_enqueue_photo(const uint8_t *jpg, size_t len,
                                const char *filter_id, int beauty)
{
    esp_err_t err = pvc_upload_enqueue(jpg, len, filter_id, beauty);
    if (err == ESP_OK && s_ev) xEventGroupSetBits(s_ev, BIT_PHOTO);
    return err;
}

void pvc_net_debug_dump(void)
{
    /* 工程模式 (§6): token 前 8 位 + 状态 + 队列深度 + 堆余量 */
    const char *tok = pvc_store_token();
    char tok8[9] = "--------";
    if (tok[0]) {
        strncpy(tok8, tok, 8);
        tok8[8] = '\0';
    }
    printf("[FW] ENG token=%s.. state=%d queue=%d boot=%lu heap=%lu\n",
           tok8, (int)s_state, pvc_upload_depth(),
           (unsigned long)pvc_store_boot_count(),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
}
