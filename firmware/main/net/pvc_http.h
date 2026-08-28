/*
 * pvc_http.h - HTTPS 请求封装 (规范 §0 通用约定)
 *
 *  - URL = PVC_API_BASE + path
 *  - TLS: esp_crt_bundle_attach (§6: 禁止 skip_cert_verify)
 *  - 定长 Content-Length, 不使用 chunked (§2)
 *  - 自动 Authorization: Bearer <token> (auth=true 且已配对时)
 *  - 每次请求输出联调日志: [FW] METHOD PATH STATUS MS (docs/04 playbook)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;
    const char *val;
} pvc_hdr_t;

typedef struct {
    const char      *method;        /* "GET" / "POST" / "DELETE" */
    const char      *path;          /* 以 / 开头, 可含查询串; 拼在 PVC_API_BASE 后 */
    bool             auth;          /* true: 附带 Bearer token (配对接口传 false) */
    const char      *content_type;  /* NULL = 不带 body */
    const uint8_t   *body;
    size_t           body_len;
    const pvc_hdr_t *headers;       /* 额外请求头, 可 NULL */
    int              n_headers;
    int              timeout_ms;    /* 0 = 默认 15000 */
} pvc_http_req_t;

typedef struct {
    int    status;      /* HTTP 状态码; <0 表示传输层失败 */
    char  *buf;         /* 调用方提供的响应缓冲 (NUL 结尾) */
    size_t cap;
    size_t len;
} pvc_http_resp_t;

/*
 * 执行请求。返回 ESP_OK 表示拿到了 HTTP 响应 (无论状态码);
 * 传输层失败 (DNS/TCP/TLS/超时) 返回错误码且 resp->status = -1。
 */
esp_err_t pvc_http_request(const pvc_http_req_t *req, pvc_http_resp_t *resp);

/* 从错误响应体 JSON 中提取整数字段 (如 retry_after); 找不到返回 fallback */
long pvc_http_json_long(const char *body, const char *field, long fallback);

#ifdef __cplusplus
}
#endif
