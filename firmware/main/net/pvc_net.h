/*
 * pvc_net.h - Presence Card 联网层总入口
 *
 * 权威规范: PresenceCardVenture/docs/02-device-api-v1.md
 * 覆盖 §1 配对 / §2 拍照上传 (幂等 + 退避 + 待传队列) / §6 checklist 网络项。
 * §3 feed 拉取与下发显示为下一阶段。
 *
 * 状态机 (pvc_net_task):
 *   WIFI_CONNECTING -> (NVS 有 token ? ONLINE : PAIRING -> ONLINE)
 *   ONLINE:  循环排空待传队列; 收到 401 -> 清 token 回 PAIRING
 *
 * 所有 UI 回调内部自行 bsp_display_lock, 可从任意任务调用。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 编译期配置 (platformio.ini build_flags 覆盖) */
#ifndef PVC_API_BASE
#define PVC_API_BASE "https://api-dev.example.com/v1"
#endif
#ifndef FW_VERSION
#define FW_VERSION "0.2.0-dev"
#endif
#ifndef PVC_WIFI_SSID
#define PVC_WIFI_SSID ""
#endif
#ifndef PVC_WIFI_PASS
#define PVC_WIFI_PASS ""
#endif
#define PVC_HW_MODEL "cores3"

typedef enum {
    PVC_NET_IDLE = 0,
    PVC_NET_PROVISIONING,   /* BLE 配网中 (首次使用 / 凭据被清除) */
    PVC_NET_WIFI_CONNECTING,
    PVC_NET_PAIRING,
    PVC_NET_ONLINE,
    PVC_NET_OFFLINE,        /* wifi 断开 / 长期失败, 队列保留 */
} pvc_net_state_t;

typedef struct {
    /* 屏幕显示 6 位配对码 (规范 §1); code 生存期仅在调用内, 需自行复制 */
    void (*show_pair_code)(const char *code);
    void (*hide_pair_code)(void);          /* 同时用于收起配网引导页 */
    /* BLE 配网引导页: 显示广播名与 POP 校验码 */
    void (*show_prov)(const char *ble_name, const char *pop);
    /* 状态栏短文案, 如 "BLE" "WiFi..." "Pair" "Online" "Off" */
    void (*status)(pvc_net_state_t st, const char *detail);
    /* feed 更新通知 (§3): total=缓存总数, fresh=本次新增 */
    void (*feed_update)(int total, int fresh);
} pvc_net_ui_t;

/* 启动联网任务 (NVS init + WiFi STA + 配对 + 上传队列)。ui 可为 NULL。 */
esp_err_t pvc_net_start(const pvc_net_ui_t *ui);

/*
 * 照片入待传队列 (拍照流程调用, LVGL 任务上下文安全)。
 * jpg: 320x240 JPEG (§2, ≤100KB); filter_id: 规范 §5 登记 id ("none"/"warm"/...)。
 * 有 SD 卡时落盘 /sdcard/queue 断电不丢; 否则驻留 PSRAM (重启丢失)。
 */
esp_err_t pvc_net_enqueue_photo(const uint8_t *jpg, size_t len, const char *filter_id);

/* 请求立即拉取 feed / 发送积压点赞 (UI 打开好友页或点赞后调用) */
void pvc_net_signal_feed(void);

/*
 * 本轮同步是否已完成 (省电判据, §6 单次唤醒流程):
 * 在线 + 待传队列已排空 + 本次启动后 feed 至少成功轮询过一次。
 */
bool pvc_net_synced(void);

/* 工程模式 (§6): 打印 token 前 8 位 / 状态 / 队列深度 / 堆余量 */
void pvc_net_debug_dump(void);

pvc_net_state_t pvc_net_state(void);

#ifdef __cplusplus
}
#endif
