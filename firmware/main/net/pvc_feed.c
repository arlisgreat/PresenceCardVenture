#include "pvc_feed.h"
#include "pvc_config.h"
#include "pvc_http.h"
#include "pvc_store.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "pvc_feed";

#define FEED_DIR      "/sdcard/feed"
#define IDX_PATH      FEED_DIR "/feed.idx"
#define JPEG_CAP      (160 * 1024)     /* 单张下载上限 (规范硬上限 1MB, 实际 ≤100KB) */
#define META_RESP_CAP (8 * 1024)       /* /feed JSON 响应 (8 条元数据) */

#define PSRAM_MALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef struct {
    pvc_feed_item_t meta;
    uint8_t *jpg;              /* PSRAM */
    size_t   len;
} slot_t;

static slot_t s_slots[PVC_FEED_MAX];
static int    s_count;
static SemaphoreHandle_t s_lock;

/* 反应异步队列 (§3.4) */
typedef struct {
    char photo_id[48];
    char type[12];
} react_req_t;
static react_req_t s_reacts[4];
static int s_react_n;

static void lock_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

/* ---------------- SD 镜像 ---------------- */
static void sd_save_slot(const slot_t *s)
{
    char path[96];
    snprintf(path, sizeof(path), FEED_DIR "/%s.jpg", s->meta.photo_id);
    FILE *f = fopen(path, "wb");
    if (!f) return;                       /* 无 SD: 静默跳过 (纯 PSRAM 模式) */
    fwrite(s->jpg, 1, s->len, f);
    fclose(f);

    snprintf(path, sizeof(path), FEED_DIR "/%s.txt", s->meta.photo_id);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n%s\n%s\n", s->meta.author, s->meta.caption, s->meta.filter);
    fclose(f);
}

static void sd_sync_index(void)
{
    mkdir(FEED_DIR, 0755);
    FILE *f = fopen(IDX_PATH, "w");
    if (!f) return;
    for (int i = 0; i < s_count; i++) fprintf(f, "%s\n", s_slots[i].meta.photo_id);
    fclose(f);

    /* 清理不在当前列表中的旧图 (只保最近 8 张, §3.3) */
    DIR *d = opendir(FEED_DIR);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *dot = strrchr(ent->d_name, '.');
        if (!dot || (strcmp(dot, ".jpg") != 0 && strcmp(dot, ".txt") != 0)) continue;
        char id[48] = { 0 };
        size_t idlen = (size_t)(dot - ent->d_name);
        if (idlen >= sizeof(id)) continue;
        memcpy(id, ent->d_name, idlen);
        bool keep = false;
        for (int i = 0; i < s_count && !keep; i++) {
            keep = (strcmp(s_slots[i].meta.photo_id, id) == 0);
        }
        if (!keep) {
            char path[96];
            char name[64];
            strncpy(name, ent->d_name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            snprintf(path, sizeof(path), FEED_DIR "/%s", name);
            remove(path);
        }
    }
    closedir(d);
}

void pvc_feed_init(void)
{
    lock_init();
    FILE *idx = fopen(IDX_PATH, "r");
    if (!idx) return;

    char line[64];
    int n = 0;
    while (n < PVC_FEED_MAX && fgets(line, sizeof(line), idx)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;

        char path[96];
        snprintf(path, sizeof(path), FEED_DIR "/%s.jpg", line);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > JPEG_CAP) { fclose(f); continue; }
        uint8_t *buf = PSRAM_MALLOC((size_t)sz);
        if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
            if (buf) heap_caps_free(buf);
            fclose(f);
            continue;
        }
        fclose(f);

        slot_t *s = &s_slots[n];
        memset(s, 0, sizeof(*s));
        strncpy(s->meta.photo_id, line, sizeof(s->meta.photo_id) - 1);
        s->jpg = buf;
        s->len = (size_t)sz;

        snprintf(path, sizeof(path), FEED_DIR "/%s.txt", line);
        f = fopen(path, "r");
        if (f) {
            if (fgets(s->meta.author, sizeof(s->meta.author), f))
                s->meta.author[strcspn(s->meta.author, "\r\n")] = '\0';
            if (fgets(s->meta.caption, sizeof(s->meta.caption), f))
                s->meta.caption[strcspn(s->meta.caption, "\r\n")] = '\0';
            if (fgets(s->meta.filter, sizeof(s->meta.filter), f))
                s->meta.filter[strcspn(s->meta.filter, "\r\n")] = '\0';
            fclose(f);
        }
        n++;
    }
    fclose(idx);
    s_count = n;
    if (n) ESP_LOGI(TAG, "restored %d cached feed photo(s)", n);
}

/* ---------------- 拉取 ---------------- */
/* GET /device/state (§3.1); 返回 unseen_count, 错误 <0。
 * 同时把响应交给 pvc_config 处理 web 下发的 pending_config。 */
static int fetch_state(void)
{
    char rbuf[2048];    /* 含 pending_config/active_config, 1KB 不够 */
    pvc_http_req_t req = { .method = "GET", .path = "/device/state", .auth = true };
    pvc_http_resp_t resp = { .buf = rbuf, .cap = sizeof(rbuf) };
    if (pvc_http_request(&req, &resp) != ESP_OK) return PVC_FEED_ERR;
    if (resp.status == 401) return PVC_FEED_AUTH;
    if (resp.status != 200) return PVC_FEED_ERR;

    int unseen = 0;
    cJSON *j = cJSON_Parse(rbuf);
    if (!j) return PVC_FEED_ERR;
    const cJSON *ju = cJSON_GetObjectItem(j, "unseen_count");
    if (cJSON_IsNumber(ju)) unseen = ju->valueint;
    pvc_config_handle_state((const struct cJSON *)j);
    cJSON_Delete(j);
    return unseen;
}

/* 下载单张 (§3.3): GET /photos/{id}/image -> PSRAM; 返回长度, 失败 <0 */
static int fetch_image(const char *photo_id, uint8_t *buf, size_t cap)
{
    char path[96];
    snprintf(path, sizeof(path), "/photos/%s/image", photo_id);
    pvc_http_req_t req = { .method = "GET", .path = path, .auth = true,
                           .timeout_ms = 30000 };
    pvc_http_resp_t resp = { .buf = (char *)buf, .cap = cap };
    if (pvc_http_request(&req, &resp) != ESP_OK) return -1;
    if (resp.status != 200) return -1;
    return (int)resp.len;
}

static void copy_json_str(char *dst, size_t cap, const cJSON *j)
{
    if (cJSON_IsString(j)) {
        strncpy(dst, j->valuestring, cap - 1);
        dst[cap - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

int pvc_feed_poll(void)
{
    lock_init();

    int unseen = fetch_state();
    if (unseen < 0) return unseen;
    /* 无未读且本地已有缓存 -> 不拉 feed (§3.1); 缓存为空则强制拉一次 */
    if (unseen == 0 && s_count > 0) return 0;

    /* GET /feed (§3.2), 带 If-None-Match */
    char *rbuf = PSRAM_MALLOC(META_RESP_CAP);
    if (!rbuf) return PVC_FEED_ERR;
    pvc_hdr_t hdr = { "If-None-Match", pvc_store_etag() };
    pvc_http_req_t req = {
        .method = "GET", .path = "/feed?limit=8", .auth = true,
        .headers = &hdr, .n_headers = pvc_store_etag()[0] ? 1 : 0,
    };
    pvc_http_resp_t resp = { .buf = rbuf, .cap = META_RESP_CAP };
    esp_err_t herr = pvc_http_request(&req, &resp);
    if (herr != ESP_OK || resp.status == 304) {
        heap_caps_free(rbuf);
        return (herr == ESP_OK) ? 0 : PVC_FEED_ERR;
    }
    if (resp.status == 401) { heap_caps_free(rbuf); return PVC_FEED_AUTH; }
    if (resp.status != 200) { heap_caps_free(rbuf); return PVC_FEED_ERR; }

    cJSON *j = cJSON_Parse(rbuf);
    heap_caps_free(rbuf);
    if (!j) return PVC_FEED_ERR;

    const cJSON *items = cJSON_GetObjectItem(j, "items");
    int n_items = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    if (n_items > PVC_FEED_MAX) n_items = PVC_FEED_MAX;

    /* 构建新槽位: 已缓存的复用 JPEG, 新条目才下载 */
    slot_t fresh[PVC_FEED_MAX];
    memset(fresh, 0, sizeof(fresh));
    int n_ok = 0, n_new = 0;

    for (int i = 0; i < n_items; i++) {
        const cJSON *it = cJSON_GetArrayItem(items, i);
        slot_t *s = &fresh[n_ok];
        copy_json_str(s->meta.photo_id, sizeof(s->meta.photo_id),
                      cJSON_GetObjectItem(it, "photo_id"));
        if (!s->meta.photo_id[0]) continue;
        const cJSON *au = cJSON_GetObjectItem(it, "author");
        copy_json_str(s->meta.author, sizeof(s->meta.author),
                      au ? cJSON_GetObjectItem(au, "display_name") : NULL);
        copy_json_str(s->meta.caption, sizeof(s->meta.caption),
                      cJSON_GetObjectItem(it, "caption"));
        copy_json_str(s->meta.filter, sizeof(s->meta.filter),
                      cJSON_GetObjectItem(it, "filter_id"));

        /* 命中现有缓存: 转移 JPEG 所有权 */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int k = 0; k < s_count; k++) {
            if (s_slots[k].jpg &&
                strcmp(s_slots[k].meta.photo_id, s->meta.photo_id) == 0) {
                s->jpg = s_slots[k].jpg;
                s->len = s_slots[k].len;
                s_slots[k].jpg = NULL;
                break;
            }
        }
        xSemaphoreGive(s_lock);

        if (!s->jpg) {
            uint8_t *buf = PSRAM_MALLOC(JPEG_CAP);
            if (!buf) continue;
            int len = fetch_image(s->meta.photo_id, buf, JPEG_CAP);
            if (len <= 0) { heap_caps_free(buf); continue; }
            /* 收缩到实际大小 */
            uint8_t *tight = PSRAM_MALLOC((size_t)len);
            if (tight) {
                memcpy(tight, buf, (size_t)len);
                heap_caps_free(buf);
                s->jpg = tight;
            } else {
                s->jpg = buf;
            }
            s->len = (size_t)len;
            n_new++;
            sd_save_slot(s);
        }
        n_ok++;
    }

    /* 原子替换缓存, 释放未复用的旧图 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 0; k < s_count; k++) {
        if (s_slots[k].jpg) heap_caps_free(s_slots[k].jpg);
    }
    memcpy(s_slots, fresh, sizeof(s_slots));
    s_count = n_ok;
    xSemaphoreGive(s_lock);

    const cJSON *je = cJSON_GetObjectItem(j, "etag");
    if (cJSON_IsString(je)) pvc_store_set_etag(je->valuestring);
    cJSON_Delete(j);

    sd_sync_index();
    ESP_LOGI(TAG, "feed updated: %d item(s), %d new", n_ok, n_new);
    return n_new;
}

/* ---------------- 读取 (任意任务) ---------------- */
int pvc_feed_snapshot(pvc_feed_item_t *out, int max)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count < max ? s_count : max;
    for (int i = 0; i < n; i++) out[i] = s_slots[i].meta;
    xSemaphoreGive(s_lock);
    return n;
}

int pvc_feed_read_jpeg(const char *photo_id, uint8_t *buf, size_t cap)
{
    lock_init();
    int ret = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (s_slots[i].jpg && strcmp(s_slots[i].meta.photo_id, photo_id) == 0) {
            if (s_slots[i].len <= cap) {
                memcpy(buf, s_slots[i].jpg, s_slots[i].len);
                ret = (int)s_slots[i].len;
            }
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return ret;
}

/* ---------------- 反应 (§3.4) ---------------- */
void pvc_feed_react_async(const char *photo_id, const char *type)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_react_n < (int)(sizeof(s_reacts) / sizeof(s_reacts[0]))) {
        react_req_t *r = &s_reacts[s_react_n++];
        strncpy(r->photo_id, photo_id, sizeof(r->photo_id) - 1);
        r->photo_id[sizeof(r->photo_id) - 1] = '\0';
        strncpy(r->type, type, sizeof(r->type) - 1);
        r->type[sizeof(r->type) - 1] = '\0';
    }
    xSemaphoreGive(s_lock);
}

int pvc_feed_process_reactions(void)
{
    lock_init();
    for (;;) {
        react_req_t r;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_react_n == 0) {
            xSemaphoreGive(s_lock);
            return 0;
        }
        r = s_reacts[0];
        s_react_n--;
        memmove(&s_reacts[0], &s_reacts[1], sizeof(r) * (size_t)s_react_n);
        xSemaphoreGive(s_lock);

        char path[96], body[32], rbuf[256];
        snprintf(path, sizeof(path), "/photos/%s/reactions", r.photo_id);
        snprintf(body, sizeof(body), "{\"type\":\"%s\"}", r.type);
        pvc_http_req_t req = {
            .method = "POST", .path = path, .auth = true,
            .content_type = "application/json",
            .body = (const uint8_t *)body, .body_len = strlen(body),
        };
        pvc_http_resp_t resp = { .buf = rbuf, .cap = sizeof(rbuf) };
        if (pvc_http_request(&req, &resp) != ESP_OK) continue;   /* 网络错: 丢弃 */
        if (resp.status == 401) return PVC_FEED_AUTH;
        /* 201/其它: 点赞尽力而为, 不重试 */
    }
}
