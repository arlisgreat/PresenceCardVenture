/*
 * pvc_prov.h - BLE 配网 (ESP-IDF wifi_provisioning manager + NimBLE)
 *
 * 手机侧用 Espressif "ESP BLE Provisioning" App (iOS/Android) 或
 * 集成 esp-idf-provisioning SDK 的自研 App:
 *   1. 扫描 BLE 广播名 (屏幕显示, 形如 PVC_a1b2c3)
 *   2. 输入 POP 校验码 (屏幕显示; Security1 加密握手)
 *   3. 下发 Wi-Fi SSID/密码 -> 设备存 NVS 并自动连接
 * 串口同时打印标准配网 QR JSON, 可供 App 扫码直连。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "pvc_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 是否已有 Wi-Fi 凭据 (NVS)。须在 esp_wifi_init 之后调用。 */
bool pvc_prov_is_provisioned(void);

/*
 * 启动 BLE 配网并阻塞至拿到凭据 (manager 配好后自动发起 STA 连接)。
 * ui 用于屏显 BLE 名称与 POP; 凭据错误自动复位状态机允许手机重试。
 */
esp_err_t pvc_prov_run(const pvc_net_ui_t *ui);

/* 清除已存 Wi-Fi 凭据 (换网时用); 下次启动将重新进入 BLE 配网 */
void pvc_prov_reset(void);

#ifdef __cplusplus
}
#endif
