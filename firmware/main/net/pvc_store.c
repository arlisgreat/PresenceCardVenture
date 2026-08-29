#include "pvc_store.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "pvc_store";

#define NVS_NS       "pvc"
#define KEY_TOKEN    "token"
#define KEY_BOOTCNT  "boot_cnt"
#define KEY_ETAG     "feed_etag"
#define KEY_CFG      "cfg_id"
#define KEY_OTA_BAD  "ota_bad"     /* 升级后被回滚的坏版本 (黑名单) */
#define KEY_OTA_TRY  "ota_try"     /* 已写槽待验证的版本 (回滚归因用) */

static nvs_handle_t s_nvs;
static char     s_device_id[20];               /* dvc_ + 12 hex + NUL */
static char     s_token[PVC_TOKEN_MAX];
static char     s_etag[PVC_ETAG_MAX];
static char     s_cfg_id[PVC_CFG_ID_MAX];
static char     s_ota_bad[PVC_FW_VER_MAX];
static char     s_ota_try[PVC_FW_VER_MAX];
static uint32_t s_boot_cnt;
static uint32_t s_photo_seq;                   /* RAM, 每次启动归零 */

esp_err_t pvc_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    /* device_id = dvc_ + STA MAC (规范 §1.1: 由 MAC 派生, 终身不变) */
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "dvc_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    size_t len = sizeof(s_token);
    if (nvs_get_str(s_nvs, KEY_TOKEN, s_token, &len) != ESP_OK) {
        s_token[0] = '\0';
    }
    len = sizeof(s_etag);
    if (nvs_get_str(s_nvs, KEY_ETAG, s_etag, &len) != ESP_OK) {
        s_etag[0] = '\0';
    }
    len = sizeof(s_cfg_id);
    if (nvs_get_str(s_nvs, KEY_CFG, s_cfg_id, &len) != ESP_OK) {
        s_cfg_id[0] = '\0';
    }

    len = sizeof(s_ota_bad);
    if (nvs_get_str(s_nvs, KEY_OTA_BAD, s_ota_bad, &len) != ESP_OK) {
        s_ota_bad[0] = '\0';
    }
    len = sizeof(s_ota_try);
    if (nvs_get_str(s_nvs, KEY_OTA_TRY, s_ota_try, &len) != ESP_OK) {
        s_ota_try[0] = '\0';
    }

    nvs_get_u32(s_nvs, KEY_BOOTCNT, &s_boot_cnt);
    s_boot_cnt++;
    nvs_set_u32(s_nvs, KEY_BOOTCNT, s_boot_cnt);
    nvs_commit(s_nvs);

    ESP_LOGI(TAG, "device_id=%s boot=%lu token=%s", s_device_id,
             (unsigned long)s_boot_cnt, s_token[0] ? "yes" : "none");
    return ESP_OK;
}

const char *pvc_store_device_id(void) { return s_device_id; }
const char *pvc_store_token(void)     { return s_token; }

esp_err_t pvc_store_set_token(const char *token)
{
    strncpy(s_token, token, sizeof(s_token) - 1);
    s_token[sizeof(s_token) - 1] = '\0';
    esp_err_t err = nvs_set_str(s_nvs, KEY_TOKEN, s_token);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    return err;
}

void pvc_store_clear_token(void)
{
    s_token[0] = '\0';
    nvs_erase_key(s_nvs, KEY_TOKEN);
    nvs_commit(s_nvs);
}

uint32_t pvc_store_boot_count(void)     { return s_boot_cnt; }
uint32_t pvc_store_next_photo_seq(void) { return ++s_photo_seq; }

const char *pvc_store_last_cfg(void) { return s_cfg_id; }

void pvc_store_set_last_cfg(const char *cfg_id)
{
    strncpy(s_cfg_id, cfg_id, sizeof(s_cfg_id) - 1);
    s_cfg_id[sizeof(s_cfg_id) - 1] = '\0';
    nvs_set_str(s_nvs, KEY_CFG, s_cfg_id);
    nvs_commit(s_nvs);
}

static void set_str(const char *key, char *dst, size_t cap, const char *val)
{
    strncpy(dst, val ? val : "", cap - 1);
    dst[cap - 1] = '\0';
    if (dst[0]) nvs_set_str(s_nvs, key, dst);
    else        nvs_erase_key(s_nvs, key);
    nvs_commit(s_nvs);
}

const char *pvc_store_ota_bad(void) { return s_ota_bad; }
const char *pvc_store_ota_try(void) { return s_ota_try; }

void pvc_store_set_ota_bad(const char *ver)
{
    set_str(KEY_OTA_BAD, s_ota_bad, sizeof(s_ota_bad), ver);
}

void pvc_store_set_ota_try(const char *ver)
{
    set_str(KEY_OTA_TRY, s_ota_try, sizeof(s_ota_try), ver);
}

const char *pvc_store_etag(void) { return s_etag; }

void pvc_store_set_etag(const char *etag)
{
    strncpy(s_etag, etag, sizeof(s_etag) - 1);
    s_etag[sizeof(s_etag) - 1] = '\0';
    nvs_set_str(s_nvs, KEY_ETAG, s_etag);
    nvs_commit(s_nvs);
}
