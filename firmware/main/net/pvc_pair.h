/*
 * pvc_pair.h - 设备配对状态机 (规范 §1)
 *
 * POST /pair/code 领码 -> UI 显示 -> 每 3s GET /pair/status 轮询
 * -> 200 bound: token 写 NVS; 410 过期: 重新领码。
 */
#pragma once

#include "esp_err.h"
#include "pvc_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 阻塞执行配对直至拿到 token (已存 NVS) 或不可恢复失败。
 * ui 用于显示/隐藏配对码, 可为 NULL。
 * 返回 ESP_OK = 已绑定; ESP_FAIL = 连续失败放弃 (调用方稍后重试)。
 */
esp_err_t pvc_pair_run(const pvc_net_ui_t *ui);

#ifdef __cplusplus
}
#endif
