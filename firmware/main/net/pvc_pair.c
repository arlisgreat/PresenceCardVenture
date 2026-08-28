#include "pvc_pair.h"
#include "pvc_http.h"
#include "pvc_store.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "pvc_pair";

#define POLL_INTERVAL_MS 3000       /* §1: 每 3 秒 */
#define RESP_CAP         768

/* POST /pair/code -> pair_code / expires_in (§1.1) */
static esp_err_t request_code(char *code, size_t code_cap, int *expires_s)
{
    char body[128], rbuf[RESP_CAP];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"fw_version\":\"%s\",\"hw\":\"%s\"}",
             pvc_store_device_id(), FW_VERSION, PVC_HW_MODEL);

    pvc_http_req_t req = {
        .method = "POST", .path = "/pair/code", .auth = false,
        .content_type = "application/json",
        .body = (const uint8_t *)body, .body_len = strlen(body),
    };
    pvc_http_resp_t resp = { .buf = rbuf, .cap = sizeof(rbuf) };
    if (pvc_http_request(&req, &resp) != ESP_OK || resp.status != 200) {
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    cJSON *j = cJSON_Parse(rbuf);
    if (j) {
        const cJSON *jc = cJSON_GetObjectItem(j, "pair_code");
        const cJSON *je = cJSON_GetObjectItem(j, "expires_in");
        if (cJSON_IsString(jc)) {
            strncpy(code, jc->valuestring, code_cap - 1);
            code[code_cap - 1] = '\0';
            *expires_s = cJSON_IsNumber(je) ? je->valueint : 600;
            ret = ESP_OK;
        }
        cJSON_Delete(j);
    }
    return ret;
}

/*
 * GET /pair/status 轮询一次 (§1.2)。
 * 返回: ESP_OK=已绑定(token 已存) / ESP_ERR_NOT_FINISHED=pending
 *       ESP_ERR_TIMEOUT=码过期(410) / ESP_FAIL=其它
 */
static esp_err_t poll_status(const char *code)
{
    char path[128], rbuf[RESP_CAP];
    snprintf(path, sizeof(path), "/pair/status?device_id=%s&pair_code=%s",
             pvc_store_device_id(), code);

    pvc_http_req_t req = { .method = "GET", .path = path, .auth = false };
    pvc_http_resp_t resp = { .buf = rbuf, .cap = sizeof(rbuf) };
    if (pvc_http_request(&req, &resp) != ESP_OK) return ESP_FAIL;

    if (resp.status == 202) return ESP_ERR_NOT_FINISHED;
    if (resp.status == 410) return ESP_ERR_TIMEOUT;
    if (resp.status != 200) return ESP_FAIL;

    esp_err_t ret = ESP_FAIL;
    cJSON *j = cJSON_Parse(rbuf);
    if (j) {
        const cJSON *jt = cJSON_GetObjectItem(j, "device_token");
        if (cJSON_IsString(jt) && pvc_store_set_token(jt->valuestring) == ESP_OK) {
            const cJSON *ju = cJSON_GetObjectItem(j, "user");
            const cJSON *jn = ju ? cJSON_GetObjectItem(ju, "username") : NULL;
            ESP_LOGI(TAG, "bound to user=%s",
                     cJSON_IsString(jn) ? jn->valuestring : "?");
            ret = ESP_OK;
        }
        cJSON_Delete(j);
    }
    return ret;
}

esp_err_t pvc_pair_run(const pvc_net_ui_t *ui)
{
    /* 外层: 领码失败重试 (指数退避 1s->4s->15s, §0); 码过期自动重领 */
    for (int code_round = 0; code_round < 8; code_round++) {
        char code[16];
        int expires_s = 600;
        static const int backoff_ms[] = { 1000, 4000, 15000 };
        bool got = false;
        for (int i = 0; i < 3 && !got; i++) {
            got = (request_code(code, sizeof(code), &expires_s) == ESP_OK);
            if (!got) vTaskDelay(pdMS_TO_TICKS(backoff_ms[i]));
        }
        if (!got) return ESP_FAIL;

        ESP_LOGI(TAG, "pair_code=%s expires=%ds", code, expires_s);
        if (ui && ui->show_pair_code) ui->show_pair_code(code);

        int rounds = expires_s * 1000 / POLL_INTERVAL_MS;
        for (int i = 0; i < rounds; i++) {
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            esp_err_t st = poll_status(code);
            if (st == ESP_OK) {
                if (ui && ui->hide_pair_code) ui->hide_pair_code();
                return ESP_OK;
            }
            if (st == ESP_ERR_TIMEOUT) break;      /* 410 -> 重新领码 */
            /* pending / 偶发网络错: 继续轮询 */
        }
    }
    if (ui && ui->hide_pair_code) ui->hide_pair_code();
    return ESP_FAIL;
}
