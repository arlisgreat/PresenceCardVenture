#include "pvc_http.h"
#include "pvc_net.h"
#include "pvc_store.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "pvc_http";

static esp_http_client_method_t method_of(const char *m)
{
    if (strcmp(m, "POST") == 0)   return HTTP_METHOD_POST;
    if (strcmp(m, "DELETE") == 0) return HTTP_METHOD_DELETE;
    return HTTP_METHOD_GET;
}

esp_err_t pvc_http_request(const pvc_http_req_t *req, pvc_http_resp_t *resp)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", PVC_API_BASE, req->path);

    resp->status = -1;
    resp->len = 0;
    if (resp->cap) resp->buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method_of(req->method),
        .timeout_ms = req->timeout_ms ? req->timeout_ms : 15000,
        /* §6: Let's Encrypt / ISRG Root X1 已含于 IDF 证书 bundle */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;

    if (req->auth && pvc_store_token()[0]) {
        char bearer[PVC_TOKEN_MAX + 8];
        snprintf(bearer, sizeof(bearer), "Bearer %s", pvc_store_token());
        esp_http_client_set_header(c, "Authorization", bearer);
    }
    if (req->content_type) {
        esp_http_client_set_header(c, "Content-Type", req->content_type);
    }
    for (int i = 0; i < req->n_headers; i++) {
        esp_http_client_set_header(c, req->headers[i].key, req->headers[i].val);
    }

    int64_t t0 = esp_timer_get_time();
    /* open(定长)/write/fetch/read: 显式 Content-Length, 规避 chunked (§2) */
    esp_err_t err = esp_http_client_open(c, (int)req->body_len);
    if (err == ESP_OK && req->body_len) {
        int wr = esp_http_client_write(c, (const char *)req->body, (int)req->body_len);
        if (wr != (int)req->body_len) err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        if (esp_http_client_fetch_headers(c) < 0) {
            err = ESP_FAIL;
        } else {
            resp->status = esp_http_client_get_status_code(c);
            while (resp->len + 1 < resp->cap) {
                int rd = esp_http_client_read(c,
                                              resp->buf + resp->len,
                                              (int)(resp->cap - 1 - resp->len));
                if (rd <= 0) break;
                resp->len += (size_t)rd;
            }
            resp->buf[resp->len] = '\0';
        }
    }
    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    /* 联调日志 (格式固定, 全栈复现用): [FW] METHOD PATH STATUS MS */
    printf("[FW] %s %s %d %lu\n", req->method, req->path,
           resp->status, (unsigned long)ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s transport error: %s", req->method, req->path,
                 esp_err_to_name(err));
    }
    return err;
}

long pvc_http_json_long(const char *body, const char *field, long fallback)
{
    /* 轻量提取: "field": <num> —— 错误体很小, 不值得整棵 cJSON 解析 */
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", field);
    const char *p = strstr(body, pat);
    if (!p) return fallback;
    p += strlen(pat);
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    return (end && end != p) ? v : fallback;
}
