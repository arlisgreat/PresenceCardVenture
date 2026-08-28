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

/*
 * 持久连接: 单个 esp_http_client 句柄跨请求复用 (keep-alive), 省去每次
 * 1-2s 的 TLS 握手 —— 单次唤醒 4-6 个请求时在线时长约砍半。
 * 约束: 所有调用方均在 pvc_net 任务 (单线程), 不加锁; 新增调用方必须同任务。
 * 服务器闲置断开由重试兜底: open/写/读失败 -> 重建句柄整单重试一次。
 */
static esp_http_client_handle_t s_client;
static bool s_conn_fresh;          /* 本次请求是否新建了连接 (日志用) */

static esp_http_client_method_t method_of(const char *m)
{
    if (strcmp(m, "POST") == 0)   return HTTP_METHOD_POST;
    if (strcmp(m, "DELETE") == 0) return HTTP_METHOD_DELETE;
    return HTTP_METHOD_GET;
}

static void client_drop(void)
{
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

static esp_http_client_handle_t client_get(void)
{
    if (s_client) {
        s_conn_fresh = false;
        return s_client;
    }
    esp_http_client_config_t cfg = {
        .url = PVC_API_BASE,               /* 基地址定 host; 每请求 set_url 换路径 */
        .timeout_ms = 15000,
        /* §6: Let's Encrypt / ISRG Root X1 已含于 IDF 证书 bundle */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .disable_auto_redirect = false,
    };
    s_client = esp_http_client_init(&cfg);
    s_conn_fresh = true;
    return s_client;
}

/* 本次请求设置过的头, 请求后清掉, 防止残留到下一次复用 */
#define MAX_TRACK_HDRS 12
static const char *s_set_hdrs[MAX_TRACK_HDRS];
static int s_n_set_hdrs;

static void hdr_set(esp_http_client_handle_t c, const char *k, const char *v)
{
    esp_http_client_set_header(c, k, v);
    if (s_n_set_hdrs < MAX_TRACK_HDRS) s_set_hdrs[s_n_set_hdrs++] = k;
}

static void hdrs_clear(esp_http_client_handle_t c)
{
    for (int i = 0; i < s_n_set_hdrs; i++) {
        esp_http_client_delete_header(c, s_set_hdrs[i]);
    }
    s_n_set_hdrs = 0;
}

/* 单次执行 (不含重试)。返回 ESP_OK = 拿到 HTTP 响应。 */
static esp_err_t do_request(const pvc_http_req_t *req, pvc_http_resp_t *resp)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", PVC_API_BASE, req->path);

    esp_http_client_handle_t c = client_get();
    if (!c) return ESP_ERR_NO_MEM;

    esp_http_client_set_url(c, url);
    esp_http_client_set_method(c, method_of(req->method));
    esp_http_client_set_timeout_ms(c, req->timeout_ms ? req->timeout_ms : 15000);

    if (req->auth && pvc_store_token()[0]) {
        char bearer[PVC_TOKEN_MAX + 8];
        snprintf(bearer, sizeof(bearer), "Bearer %s", pvc_store_token());
        hdr_set(c, "Authorization", bearer);
    }
    if (req->content_type) hdr_set(c, "Content-Type", req->content_type);
    for (int i = 0; i < req->n_headers; i++) {
        hdr_set(c, req->headers[i].key, req->headers[i].val);
    }

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
            /* 缓冲填满后还有数据 = 截断 (调用方按失败处理) */
            char probe;
            if (resp->len + 1 >= resp->cap &&
                esp_http_client_read(c, &probe, 1) > 0) {
                resp->truncated = true;
            }
        }
    }
    hdrs_clear(c);
    if (err != ESP_OK) {
        client_drop();                 /* 连接已不可信, 下次重建 */
    }
    /* 成功时不 close: 连接保活复用 */
    return err;
}

esp_err_t pvc_http_request(const pvc_http_req_t *req, pvc_http_resp_t *resp)
{
    resp->status = -1;
    resp->len = 0;
    resp->truncated = false;
    if (resp->cap) resp->buf[0] = '\0';

    int64_t t0 = esp_timer_get_time();
    bool was_fresh;
    esp_err_t err = do_request(req, resp);
    was_fresh = s_conn_fresh;
    if (err != ESP_OK) {
        /* 复用连接可能被服务器闲置断开: 重建后整单重试一次 */
        resp->len = 0;
        resp->truncated = false;
        if (resp->cap) resp->buf[0] = '\0';
        err = do_request(req, resp);
        was_fresh = true;
    }
    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    /* 联调日志 (格式固定, 全栈复现用): [FW] METHOD PATH STATUS MS [conn=] */
    printf("[FW] %s %s %d %lu conn=%s\n", req->method, req->path,
           resp->status, (unsigned long)ms, was_fresh ? "new" : "keep");
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
