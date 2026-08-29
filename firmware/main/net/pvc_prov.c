#include "pvc_prov.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static const char *TAG = "pvc_prov";

/* BLE 广播名 / POP 均由 MAC 派生, 与机身一一对应 */
static void derive_ids(char *name, size_t name_cap, char *pop, size_t pop_cap)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(name, name_cap, "PVC_%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(pop, pop_cap, "pvc%02x%02x", mac[4], mac[5]);
}

static void prov_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch (id) {
    case WIFI_PROV_START:
        ESP_LOGI(TAG, "provisioning started");
        break;
    case WIFI_PROV_CRED_RECV: {
        const wifi_sta_config_t *c = (const wifi_sta_config_t *)data;
        ESP_LOGI(TAG, "received ssid=%s", (const char *)c->ssid);
        break;
    }
    case WIFI_PROV_CRED_FAIL:
        /* 密码错/找不到 AP: 复位状态机, 手机可直接重试, 不用重启设备 */
        ESP_LOGW(TAG, "credentials failed, resetting for retry");
        wifi_prov_mgr_reset_sm_state_on_failure();
        break;
    case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "credentials ok");
        break;
    case WIFI_PROV_END:
        ESP_LOGI(TAG, "provisioning finished");
        break;
    default:
        break;
    }
}

bool pvc_prov_is_provisioned(void)
{
    bool provisioned = false;
    /* manager 未 init 时也可查询: 直接读 NVS 中的 STA 配置。
     * 注意: 这里绝不能挂 FREE_BTDM —— 该处理器在 deinit 时把 BT 控制器
     * 内存一次性释放 (本次启动不可恢复), 之后真配网 BLE init 必崩
     * (真机实测: btdm_controller_init 失败 -> deinit 路径 LoadProhibited) */
    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    if (wifi_prov_mgr_init(cfg) != ESP_OK) return false;
    wifi_prov_mgr_is_provisioned(&provisioned);
    wifi_prov_mgr_deinit();
    return provisioned;
}

esp_err_t pvc_prov_run(const pvc_net_ui_t *ui)
{
    char name[16], pop[12];
    derive_ids(name, sizeof(name), pop, sizeof(pop));

    esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                               prov_event_cb, NULL);

    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_ble,
        /* 配网结束释放 BT 控制器内存 (之后不再用 BLE, 省 ~60KB) */
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    esp_err_t err = wifi_prov_mgr_init(cfg);
    if (err != ESP_OK) return err;

    /* Security1: X25519 握手 + POP 校验, 凭据密文传输 */
    err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, pop, name, NULL);
    if (err != ESP_OK) {
        wifi_prov_mgr_deinit();
        return err;
    }

    /* 标准配网 QR payload (App 可扫码免手输; 串口日志供联调) */
    printf("[FW] PROV ble name=%s pop=%s\n", name, pop);
    printf("[FW] PROV qr {\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}\n",
           name, pop);
    if (ui && ui->show_prov) ui->show_prov(name, pop);

    /* 阻塞至配网完成 (manager 已自动 esp_wifi_set_config + connect) */
    wifi_prov_mgr_wait();
    wifi_prov_mgr_deinit();
    esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event_cb);
    if (ui && ui->hide_pair_code) ui->hide_pair_code();
    return ESP_OK;
}

void pvc_prov_reset(void)
{
    /* 同 is_provisioned: 探测式 init/deinit 不得挂 FREE_BTDM */
    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    if (wifi_prov_mgr_init(cfg) == ESP_OK) {
        wifi_prov_mgr_reset_provisioning();
        wifi_prov_mgr_deinit();
        ESP_LOGI(TAG, "wifi credentials erased, reboot to re-provision");
    }
}
