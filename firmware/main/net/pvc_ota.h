/*
 * pvc_ota.h - OTA 升级 (规范 §3.1 fw_latest 预留字段)
 *
 * 链路:
 *   /device/state 响应 fw_latest{version,url,md5} (server 无更新时为 null)
 *   -> 版本比较 (仅升不降, pvc_semver_cmp) + NVS 坏版本黑名单
 *   -> 联网任务空闲 (待传队列已排空) 时流式下载写入 ota_0/1 双槽
 *   -> MD5 逐块校验 + esp_ota_end 镜像校验 -> set_boot_partition
 *   -> 闲置时重启生效 (pvc_power 入睡路径改为 esp_restart)
 *
 * 回滚保护 (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE):
 *   新固件首次启动为 PENDING_VERIFY; 首次 feed 同步成功 (wifi+认证+
 *   state+feed 全通 = 自检通过) 后 pvc_ota_mark_valid() 落账;
 *   若未落账即重启/崩溃, bootloader 自动回滚旧槽。回滚发生后
 *   把该版本记入 NVS 黑名单, 防止反复升级同一坏版本。
 *
 * 线程模型: handle_state/process/mark_valid 仅在联网任务调用;
 * boot_check 在 main 早期调用; reboot_pending/busy 任意任务只读。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* main 早期调用 (pvc_store_init 之后): 识别 PENDING_VERIFY / 回滚落黑名单 */
void pvc_ota_boot_check(void);

/* feed 轮询解析完 /device/state 后调用 (state 为 cJSON 根对象) */
struct cJSON;
void pvc_ota_handle_state(const struct cJSON *state);

/* 有待下载的更新? (net_task 决定何时调 process) */
bool pvc_ota_pending(void);

/*
 * 联网任务调用: 执行下载+写槽+校验。阻塞至完成或失败。
 * notify 可 NULL: 进度短文案 ("OTA 40%") 供状态栏显示。
 * 返回 0 = 无事/已完成; PVC_FEED_AUTH(-2) = API 401 (调用方清 token)。
 */
int pvc_ota_process(void (*notify)(const char *msg));

/* 自检通过 (首次 feed 同步成功) 后调用; 幂等 */
void pvc_ota_mark_valid(void);

/* 新固件已写好待重启 (pvc_power 入睡时改为 esp_restart) */
bool pvc_ota_reboot_pending(void);

/* 下载进行中 (pvc_power 不得入睡) */
bool pvc_ota_busy(void);

#ifdef __cplusplus
}
#endif
