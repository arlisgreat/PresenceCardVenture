#include "pvc_upload.h"
#include "pvc_http.h"
#include "pvc_store.h"
#include "pvc_trace.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "pvc_upload";

#define QUEUE_DIR      "/sdcard/queue"
#define MAX_RAM_ITEMS  4              /* 无 SD 时 PSRAM 最多滞留 4 张 (~400KB) */
#define PHOTO_W        "320"
#define PHOTO_H        "240"
#define RESP_CAP       768

typedef struct up_item {
    struct up_item *next;
    char     path[80];      /* 文件路径; "" = 仅驻留 RAM */
    uint8_t *buf;           /* RAM 条目的 JPEG (PSRAM); 文件条目为 NULL */
    size_t   len;
    char     key[64];       /* Idempotency-Key */
    char     filter[16];
    int      beauty;        /* X-Beauty 0-100 (重启恢复条目为 0) */
    char     caption[96];   /* UTF-8 原文; 发送时 URL-encode (重启恢复为空) */
    char     circle[48];    /* Community 小圈; 重启恢复为“小圈” */
} up_item_t;

static up_item_t *s_head;
static SemaphoreHandle_t s_lock;
static int s_ram_items;

static void list_append(up_item_t *it)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    it->next = NULL;
    if (!s_head) {
        s_head = it;
    } else {
        up_item_t *p = s_head;
        while (p->next) p = p->next;
        p->next = it;
    }
    xSemaphoreGive(s_lock);
}

static void list_remove(up_item_t *it)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    up_item_t **pp = &s_head;
    while (*pp && *pp != it) pp = &(*pp)->next;
    if (*pp) *pp = it->next;
    if (it->buf) {
        heap_caps_free(it->buf);
        s_ram_items--;
    }
    xSemaphoreGive(s_lock);
    free(it);
}

void pvc_upload_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    /* 重启补传 (§2): 扫描遗留队列文件, 从文件名还原幂等键 */
    DIR *d = opendir(QUEUE_DIR);
    if (!d) return;                    /* 无 SD 卡 / 目录不存在: RAM 模式 */
    struct dirent *ent;
    int n = 0;
    while ((ent = readdir(d)) != NULL) {
        unsigned long boot = 0, seq = 0;
        char filter[16] = "none";
        if (sscanf(ent->d_name, "%lu_%lu_%15[a-z].jpg", &boot, &seq, filter) < 2) {
            continue;
        }
        up_item_t *it = calloc(1, sizeof(*it));
        if (!it) break;
        /* 限长复制文件名, 防 snprintf 截断告警 (-Werror=format-truncation) */
        char name[56];
        strncpy(name, ent->d_name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        snprintf(it->path, sizeof(it->path), QUEUE_DIR "/%s", name);
        snprintf(it->key, sizeof(it->key), "%s-%lu-%lu",
                 pvc_store_device_id(), boot, seq);
        strncpy(it->filter, filter, sizeof(it->filter) - 1);
        strncpy(it->circle, "小圈", sizeof(it->circle) - 1);
        list_append(it);
        n++;
    }
    closedir(d);
    if (n) ESP_LOGI(TAG, "restored %d pending photo(s) from %s", n, QUEUE_DIR);
}

esp_err_t pvc_upload_enqueue(const uint8_t *jpg, size_t len,
                             const char *filter_id, int beauty,
                             const char *caption, const char *circle)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    uint32_t boot = pvc_store_boot_count();
    uint32_t seq  = pvc_store_next_photo_seq();

    up_item_t *it = calloc(1, sizeof(*it));
    if (!it) return ESP_ERR_NO_MEM;
    snprintf(it->key, sizeof(it->key), "%s-%lu-%lu",
             pvc_store_device_id(), (unsigned long)boot, (unsigned long)seq);
    strncpy(it->filter, filter_id, sizeof(it->filter) - 1);
    it->beauty = (beauty < 0) ? 0 : (beauty > 100 ? 100 : beauty);
    if (caption) strncpy(it->caption, caption, sizeof(it->caption) - 1);
    strncpy(it->circle, (circle && circle[0]) ? circle : "小圈",
            sizeof(it->circle) - 1);

    /* 优先落盘 (断电不丢) */
    mkdir(QUEUE_DIR, 0755);
    snprintf(it->path, sizeof(it->path), QUEUE_DIR "/%lu_%lu_%s.jpg",
             (unsigned long)boot, (unsigned long)seq, it->filter);
    FILE *f = fopen(it->path, "wb");
    if (f) {
        size_t wr = fwrite(jpg, 1, len, f);
        fclose(f);
        if (wr == len) {
            list_append(it);
            PVC_EV("upload_queued key=%s bytes=%u store=sd depth=%d",
                   it->key, (unsigned)len, pvc_upload_depth());
            return ESP_OK;
        }
        remove(it->path);
    }

    /* SD 不可用: 降级 PSRAM 驻留 */
    it->path[0] = '\0';
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool full = (s_ram_items >= MAX_RAM_ITEMS);
    if (!full) s_ram_items++;
    xSemaphoreGive(s_lock);
    if (full) {
        free(it);
        ESP_LOGE(TAG, "no SD and RAM queue full, photo dropped");
        return ESP_ERR_NO_MEM;
    }
    it->buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!it->buf) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ram_items--;
        xSemaphoreGive(s_lock);
        free(it);
        return ESP_ERR_NO_MEM;
    }
    memcpy(it->buf, jpg, len);
    it->len = len;
    list_append(it);
    PVC_EV("upload_queued key=%s bytes=%u store=ram depth=%d",
           it->key, (unsigned)len, pvc_upload_depth());
    return ESP_OK;
}

/* 读出条目 JPEG (文件条目临时载入 PSRAM; 调用方负责 free 返回值当 from_file) */
static uint8_t *item_load(up_item_t *it, size_t *len, bool *from_file)
{
    if (it->buf) {
        *len = it->len;
        *from_file = false;
        return it->buf;
    }
    *from_file = true;
    FILE *f = fopen(it->path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        heap_caps_free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

/* RFC3986 百分号编码 (X-Caption 要求 URL-encode 的 UTF-8, §2) */
static void url_encode(const char *src, char *dst, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const uint8_t *p = (const uint8_t *)src; *p && o + 4 < cap; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
            *p == '.' || *p == '~') {
            dst[o++] = (char)*p;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[*p >> 4];
            dst[o++] = hex[*p & 0x0F];
        }
    }
    dst[o] = '\0';
}

/* 发送一个条目一次。返回 HTTP 状态码 (传输层失败返回 -1)。 */
static int item_post(up_item_t *it, const uint8_t *jpg, size_t len, char *rbuf, size_t rcap)
{
    char beauty_s[8], cap_enc[288], circle_enc[144];
    snprintf(beauty_s, sizeof(beauty_s), "%d", it->beauty);
    pvc_hdr_t hdrs[8];
    int nh = 0;
    hdrs[nh++] = (pvc_hdr_t){ "Idempotency-Key", it->key };
    /* server 校验 X-Device-Id 必须与 token 绑定的设备一致, 缺失即 403 */
    hdrs[nh++] = (pvc_hdr_t){ "X-Device-Id",     pvc_store_device_id() };
    hdrs[nh++] = (pvc_hdr_t){ "X-Filter-Id",     it->filter };
    hdrs[nh++] = (pvc_hdr_t){ "X-Beauty",        beauty_s };
    hdrs[nh++] = (pvc_hdr_t){ "X-Width",         PHOTO_W };
    hdrs[nh++] = (pvc_hdr_t){ "X-Height",        PHOTO_H };
    if (it->caption[0]) {
        url_encode(it->caption, cap_enc, sizeof(cap_enc));
        hdrs[nh++] = (pvc_hdr_t){ "X-Caption", cap_enc };
    }
    url_encode(it->circle[0] ? it->circle : "小圈", circle_enc, sizeof(circle_enc));
    hdrs[nh++] = (pvc_hdr_t){ "X-Circle", circle_enc };
    pvc_http_req_t req = {
        .method = "POST", .path = "/photos", .auth = true,
        .content_type = "image/jpeg",
        .body = jpg, .body_len = len,
        .headers = hdrs, .n_headers = nh,
        .timeout_ms = 30000,           /* 上传体较大, 放宽 */
    };
    pvc_http_resp_t resp = { .buf = rbuf, .cap = rcap };
    pvc_http_request(&req, &resp);
    return resp.status;
}

pvc_up_result_t pvc_upload_drain(void)
{
    static const int backoff_ms[] = { 1000, 4000, 15000 };   /* §0 */
    bool any_deferred = false;

    up_item_t *it = s_head;
    while (it) {
        up_item_t *next = it->next;
        size_t len = 0;
        bool from_file = false;
        uint8_t *jpg = item_load(it, &len, &from_file);
        if (!jpg) {
            ESP_LOGE(TAG, "load %s failed, dropping", it->path);
            if (it->path[0]) remove(it->path);
            list_remove(it);
            it = next;
            continue;
        }

        bool done = false, auth_fail = false;
        char rbuf[RESP_CAP];
        for (int attempt = 0; attempt < 3; attempt++) {
            int status = item_post(it, jpg, len, rbuf, sizeof(rbuf));
            if (status == 201 || status == 200 || status == 409) {
                /* 200/409 = 幂等键已处理过, 视为成功 (§0/§2) */
                PVC_EV("upload_sent key=%s status=%d attempt=%d", it->key, status, attempt);
                done = true;
                break;
            }
            if (status == 401) { auth_fail = true; break; }
            if (status == 400 || status == 413 || status == 415 || status == 403) {
                PVC_EV("upload_drop key=%s status=%d", it->key, status);
                ESP_LOGE(TAG, "photo rejected (%d), dropping key=%s", status, it->key);
                done = true;               /* 固件 bug 类: 丢弃防堵塞 */
                break;
            }
            if (status == 429) {
                long ra = pvc_http_json_long(rbuf, "retry_after", 60);
                if (ra > 3600) ra = 3600;
                ESP_LOGW(TAG, "rate limited, retry_after=%lds", ra);
                vTaskDelay(pdMS_TO_TICKS((uint32_t)ra * 1000));
                continue;                  /* 429 等待不计入退避次数 */
            }
            /* 5xx / 传输层失败: 指数退避后重试, 恒用同一幂等键 */
            if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(backoff_ms[attempt]));
        }
        if (from_file) heap_caps_free(jpg);

        if (auth_fail) return PVC_UP_AUTH_FAIL;
        if (done) {
            if (it->path[0]) remove(it->path);
            list_remove(it);
        } else {
            PVC_EV("upload_defer key=%s", it->key);
            any_deferred = true;           /* 留队列, 下轮再试 */
        }
        it = next;
    }
    return any_deferred ? PVC_UP_RETRY_LATER : PVC_UP_ALL_SENT;
}

int pvc_upload_depth(void)
{
    int n = 0;
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (up_item_t *p = s_head; p; p = p->next) n++;
    xSemaphoreGive(s_lock);
    return n;
}
