#include "pvc_config.h"
#include "pvc_feed.h"      /* PVC_FEED_AUTH */
#include "pvc_http.h"
#include "pvc_store.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "pvc_config";

static void (*s_cb)(const pvc_config_t *cfg);
static char s_ack_id[64];      /* 待回执的 config_id; "" = 无 */

void pvc_config_set_apply_cb(void (*cb)(const pvc_config_t *cfg))
{
    s_cb = cb;
}

static void copy_str(char *dst, size_t cap, const cJSON *j)
{
    if (cJSON_IsString(j)) {
        strncpy(dst, j->valuestring, cap - 1);
        dst[cap - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

void pvc_config_handle_state(const struct cJSON *state)
{
    const cJSON *pc = cJSON_GetObjectItem((const cJSON *)state, "pending_config");
    if (!cJSON_IsObject(pc)) return;

    pvc_config_t cfg = { 0 };
    copy_str(cfg.id, sizeof(cfg.id), cJSON_GetObjectItem(pc, "id"));
    copy_str(cfg.filter_id, sizeof(cfg.filter_id), cJSON_GetObjectItem(pc, "filter_id"));
    copy_str(cfg.play_type, sizeof(cfg.play_type), cJSON_GetObjectItem(pc, "play_type"));
    copy_str(cfg.sticker, sizeof(cfg.sticker), cJSON_GetObjectItem(pc, "sticker"));
    const cJSON *jb = cJSON_GetObjectItem(pc, "beauty");
    cfg.beauty = cJSON_IsNumber(jb) ? jb->valueint : 0;
    if (!cfg.id[0]) return;

    if (strcmp(cfg.id, pvc_store_last_cfg()) != 0) {
        ESP_LOGI(TAG, "apply config %s: filter=%s play=%s beauty=%d sticker=%s",
                 cfg.id, cfg.filter_id, cfg.play_type, cfg.beauty, cfg.sticker);
        if (s_cb) s_cb(&cfg);
        pvc_store_set_last_cfg(cfg.id);
    }
    /* 新旧配置都补发 ack: 同 id 再次出现说明上次 ack 未到达 server */
    strncpy(s_ack_id, cfg.id, sizeof(s_ack_id) - 1);
    s_ack_id[sizeof(s_ack_id) - 1] = '\0';
}

int pvc_config_process_ack(void)
{
    if (!s_ack_id[0]) return 0;

    char body[96], rbuf[256];
    snprintf(body, sizeof(body), "{\"config_id\":\"%s\"}", s_ack_id);
    pvc_http_req_t req = {
        .method = "POST", .path = "/device/ack", .auth = true,
        .content_type = "application/json",
        .body = (const uint8_t *)body, .body_len = strlen(body),
    };
    pvc_http_resp_t resp = { .buf = rbuf, .cap = sizeof(rbuf) };
    if (pvc_http_request(&req, &resp) != ESP_OK) return 0;   /* 网络错: 下轮重发 */
    if (resp.status == 401) return PVC_FEED_AUTH;
    if (resp.status >= 200 && resp.status < 300) {
        s_ack_id[0] = '\0';
    } else {
        ESP_LOGW(TAG, "ack rejected (%d), dropping config_id=%s", resp.status, s_ack_id);
        s_ack_id[0] = '\0';       /* 4xx 属协议错, 丢弃防无限重发 */
    }
    return 0;
}
