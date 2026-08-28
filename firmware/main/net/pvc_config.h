/*
 * pvc_config.h - web 下发配置与设备回执
 *
 * 链路 (server 已实现):
 *   web POST /v1/device/config -> server 存 pending_config
 *   设备 GET /v1/device/state  -> 响应带 pending_config{id,filter_id,play_type,beauty,sticker}
 *   设备应用后 POST /v1/device/ack {config_id} -> server 转为 active_config
 *
 * 幂等: NVS 记 last config id; 同 id 不重复应用, 但补发 ack
 * (覆盖 ack 丢包后 server 仍挂 pending 的情况)。
 * 线程模型: 全部仅在联网任务调用 (handle_state 由 feed 轮询触发), 无需加锁;
 * apply 回调内部自行加显示锁。
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char id[64];
    char filter_id[16];    /* none/warm/bw/film/vivid (web 清单) */
    char play_type[16];    /* beauty/ccd/template */
    char sticker[16];      /* none/star/date */
    int  beauty;           /* 0-100 */
} pvc_config_t;

/* 注册应用回调 (main 接 UI); 从联网任务调用, 回调内自行加显示锁 */
void pvc_config_set_apply_cb(void (*cb)(const pvc_config_t *cfg));

/* feed 轮询解析完 /device/state JSON 后调用 (state 为 cJSON 根对象) */
struct cJSON;
void pvc_config_handle_state(const struct cJSON *state);

/* 联网任务调用: 补发积压 ack。返回 0 正常 / PVC_FEED_AUTH(-2) 表示 401 */
int pvc_config_process_ack(void);

#ifdef __cplusplus
}
#endif
