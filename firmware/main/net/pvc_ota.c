#include "pvc_ota.h"
#include "pvc_ota_util.h"
#include "pvc_feed.h"      /* PVC_FEED_AUTH */
#include "pvc_http.h"      /* pvc_hdr_t (风格一致); 实际下载走流式 client */
#include "pvc_net.h"       /* PVC_API_BASE / FW_VERSION */
#include "pvc_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_rom_md5.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "pvc_trace.h"

static const char *TAG = "pvc_ota";

#define OTA_MAX_ATTEMPTS 3      /* 同版本连续失败次数上限, 超出即拉黑 */
#define OTA_CHUNK        4096
#define OTA_TIMEOUT_MS   30000

typedef struct {
    char    version[PVC_FW_VER_MAX];
    char    url[256];
    uint8_t md5[16];
    bool    has_md5;
} ota_info_t;

static ota_info_t s_info;
static bool s_has_pending;
static int  s_attempts;
static volatile bool s_busy;
static volatile bool s_reboot_pending;
static bool s_pending_verify;       /* 本次启动运行的是待验证新固件 */
static uint8_t s_chunk[OTA_CHUNK];  /* 下载缓冲 (BSS, 不占 net 任务栈) */

/* ---------------- 启动期: 回滚识别与自检状态 ---------------- */
void pvc_ota_boot_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    s_pending_verify = (st == ESP_OTA_IMG_PENDING_VERIFY);

    /* 上次升级后被 bootloader 回滚: 把当时写入的版本拉黑 */
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    const char *tried = pvc_store_ota_try();
    if (bad && tried[0]) {
        PVC_EV("ota_rollback bad=%s part=%s", tried, bad->label);
        pvc_store_set_ota_bad(tried);
        pvc_store_set_ota_try(NULL);
    } else if (!s_pending_verify && tried[0]) {
        /* 正常固件 + 残留 try 记录 (升级成功后 mark_valid 已清; 此处兜底) */
        pvc_store_set_ota_try(NULL);
    }
    PVC_EV("ota_boot part=%s state=%d fw=%s", run ? run->label : "?",
           (int)st, FW_VERSION);
}

void pvc_ota_mark_valid(void)
{
    if (!s_pending_verify) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_pending_verify = false;
        pvc_store_set_ota_try(NULL);
        PVC_EV("ota_valid fw=%s", FW_VERSION);
    }
}

/* ---------------- state 解析 ---------------- */
static void copy_str(char *dst, size_t cap, const cJSON *j)
{
    if (cJSON_IsString(j)) {
        strncpy(dst, j->valuestring, cap - 1);
        dst[cap - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

void pvc_ota_handle_state(const struct cJSON *state)
{
    const cJSON *fl = cJSON_GetObjectItem((const cJSON *)state, "fw_latest");
    if (!cJSON_IsObject(fl)) return;   /* null / 缺失 = 无更新 */

    ota_info_t info = { 0 };
    copy_str(info.version, sizeof(info.version), cJSON_GetObjectItem(fl, "version"));
    copy_str(info.url, sizeof(info.url), cJSON_GetObjectItem(fl, "url"));
    char md5hex[40];
    copy_str(md5hex, sizeof(md5hex), cJSON_GetObjectItem(fl, "md5"));
    info.has_md5 = (pvc_md5_hex_parse(md5hex, info.md5) == 0);

    if (!info.version[0] || !info.url[0]) return;
    if (pvc_semver_cmp(info.version, FW_VERSION) <= 0) return;   /* 仅升不降 */
    if (strcmp(info.version, pvc_store_ota_bad()) == 0) return;  /* 黑名单 */
    if (s_reboot_pending) return;                    /* 已写好槽, 等重启 */
    if (s_has_pending && strcmp(info.version, s_info.version) == 0) return;

    s_info = info;
    s_has_pending = true;
    s_attempts = 0;
    PVC_EV("ota_avail ver=%s cur=%s md5=%d", info.version, FW_VERSION,
           (int)info.has_md5);
}

bool pvc_ota_pending(void)        { return s_has_pending; }
bool pvc_ota_reboot_pending(void) { return s_reboot_pending; }
bool pvc_ota_busy(void)           { return s_busy; }

/* ---------------- 下载 + 写槽 ---------------- */
static void fail(const char *stage, int err, bool fatal)
{
    PVC_EV("ota_err stage=%s err=%d attempt=%d fatal=%d", stage, err,
           s_attempts, (int)fatal);
    if (fatal) {
        /* 镜像坏 (MD5/校验/超槽/协议 4xx): 拉黑, 等 server 换版本 */
        ESP_LOGW(TAG, "blacklisting %s (stage=%s)", s_info.version, stage);
        pvc_store_set_ota_bad(s_info.version);
        s_has_pending = false;
    } else if (s_attempts >= OTA_MAX_ATTEMPTS) {
        /* 瞬时失败 (网络/写 flash): 本次在线周期放弃, 不拉黑;
         * 下轮 /device/state 仍带 fw_latest 会重新触发 (attempts 归零) */
        ESP_LOGW(TAG, "giving up on %s this session (stage=%s)",
                 s_info.version, stage);
        s_has_pending = false;
    }
}

int pvc_ota_process(void (*notify)(const char *msg))
{
    if (!s_has_pending || s_reboot_pending) return 0;
    s_attempts++;
    int64_t t0 = esp_timer_get_time();
    PVC_EV("ota_start ver=%s attempt=%d", s_info.version, s_attempts);

    /* url 允许相对路径 (同 API 主机, 带 Bearer) 或绝对 https */
    char url[320];
    bool same_api = (s_info.url[0] == '/');
    if (same_api) snprintf(url, sizeof(url), "%s%s", PVC_API_BASE, s_info.url);
    else          snprintf(url, sizeof(url), "%s", s_info.url);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = OTA_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* §6: 禁 skip_verify */
        .buffer_size = 2048,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { fail("init", -1, false); return 0; }

    char bearer[PVC_TOKEN_MAX + 8];
    if (same_api && pvc_store_token()[0]) {
        snprintf(bearer, sizeof(bearer), "Bearer %s", pvc_store_token());
        esp_http_client_set_header(cli, "Authorization", bearer);
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t ota = 0;
    int ret = 0;
    md5_context_t md5;
    esp_rom_md5_init(&md5);

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) { fail("open", err, false); goto out_cli; }
    int64_t clen = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200) {
        if (status == 401 && same_api) ret = PVC_FEED_AUTH;
        fail("http", status, status >= 400 && status < 500 && status != 401 &&
                             status != 429);
        goto out_cli;
    }
    if (!part || (clen > 0 && clen > (int64_t)part->size)) {
        fail("size", (int)(clen / 1024), true);       /* 镜像超槽位, 无救 */
        goto out_cli;
    }
    err = esp_ota_begin(part, clen > 0 ? (size_t)clen : OTA_SIZE_UNKNOWN, &ota);
    if (err != ESP_OK) { fail("begin", err, false); goto out_cli; }

    size_t total = 0;
    int last_pct = -1;
    for (;;) {
        int n = esp_http_client_read(cli, (char *)s_chunk, sizeof(s_chunk));
        if (n < 0) { fail("read", n, false); goto out_ota; }
        if (n == 0) break;
        esp_rom_md5_update(&md5, s_chunk, (uint32_t)n);
        err = esp_ota_write(ota, s_chunk, (size_t)n);
        if (err != ESP_OK) { fail("write", err, false); goto out_ota; }
        total += (size_t)n;
        if (clen > 0) {
            int pct = (int)(total * 100 / (size_t)clen);
            if (pct / 10 != last_pct / 10) {
                last_pct = pct;
                PVC_EV("ota_progress pct=%d bytes=%u", pct, (unsigned)total);
                if (notify) {
                    char msg[24];
                    snprintf(msg, sizeof(msg), "OTA %d%%", pct);
                    notify(msg);
                }
            }
        }
    }
    if (clen > 0 && total != (size_t)clen) { fail("short", (int)total, false); goto out_ota; }
    if (total == 0) { fail("empty", 0, false); goto out_ota; }

    if (s_info.has_md5) {
        uint8_t digest[16];
        esp_rom_md5_final(digest, &md5);
        if (memcmp(digest, s_info.md5, sizeof(digest)) != 0) {
            fail("md5", 0, true);       /* 校验不符 = 资产坏, 拉黑该版本 */
            goto out_ota;
        }
    }

    err = esp_ota_end(ota);             /* 镜像魔数/哈希校验 */
    ota = 0;
    if (err != ESP_OK) { fail("verify", err, true); goto out_cli; }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) { fail("setboot", err, false); goto out_cli; }

    pvc_store_set_ota_try(s_info.version);   /* 回滚归因: 先记后启 */
    s_has_pending = false;
    s_reboot_pending = true;
    PVC_EV("ota_done ver=%s bytes=%u ms=%lld part=%s", s_info.version,
           (unsigned)total, (esp_timer_get_time() - t0) / 1000, part->label);
    if (notify) notify("OTA OK");
    goto out_cli;

out_ota:
    if (ota) esp_ota_abort(ota);
out_cli:
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    printf("[FW] OTA %s %d %lld\n", s_info.version,
           s_reboot_pending ? 200 : -1,
           (esp_timer_get_time() - t0) / 1000);
    return ret;
}
