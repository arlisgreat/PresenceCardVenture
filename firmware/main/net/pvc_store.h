/*
 * pvc_store.h - NVS 持久化 (规范 §6: token / boot 计数 / etag 存 NVS)
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PVC_TOKEN_MAX 96   /* "ak_" + 64 chars + 余量 */

/* nvs_flash_init + 打开命名空间 + boot 计数自增。必须最先调用。 */
esp_err_t pvc_store_init(void);

/* 设备 id: "dvc_" + STA MAC hex, 每次启动由 efuse 派生 (终身不变) */
const char *pvc_store_device_id(void);

/* device_token; 未配对返回 "" */
const char *pvc_store_token(void);
esp_err_t   pvc_store_set_token(const char *token);
void        pvc_store_clear_token(void);   /* 收到 401 时调用 (§0 错误表) */

uint32_t pvc_store_boot_count(void);       /* 本次启动的 boot 序号 (>=1) */
uint32_t pvc_store_next_photo_seq(void);   /* 本次启动内单调递增, 从 1 起 */

/* 最近已应用的 web 下发配置 id (幂等用); 未存返回 "" */
#define PVC_CFG_ID_MAX 64
const char *pvc_store_last_cfg(void);
void        pvc_store_set_last_cfg(const char *cfg_id);

/* OTA (§3.1 fw_latest): 坏版本黑名单 + 待验证版本 (回滚归因)。
 * 未存返回 ""; 传 NULL/"" 即清除。 */
#define PVC_FW_VER_MAX 32
const char *pvc_store_ota_bad(void);
void        pvc_store_set_ota_bad(const char *ver);
const char *pvc_store_ota_try(void);
void        pvc_store_set_ota_try(const char *ver);

/* feed 的 If-None-Match etag (§3.2); 未存返回 ""。
 * server 的 etag 为 W/"feed-<全部photo_id>" — Prisma 模式 8 个 UUID 约 300 字节,
 * 缓冲必须放大到能整存, 截断会导致 304 永不命中。 */
#define PVC_ETAG_MAX 512
const char *pvc_store_etag(void);
void        pvc_store_set_etag(const char *etag);

#ifdef __cplusplus
}
#endif
