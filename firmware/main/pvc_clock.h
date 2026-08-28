/*
 * pvc_clock.h - 计时链路: SNTP -> 系统时间 -> BM8563 外部 RTC
 *
 * CoreS3 板载 BM8563 (PCF8563 兼容, I2C 0x51, 备份电源, 断电走时)。
 * 分层:
 *   1. SNTP 联网校准系统时间 (pvc_net 启动, §0 规范要求)
 *   2. deep sleep 期间由 ESP32-S3 内部 RTC timer 保持系统时间
 *   3. 冷启动无网时从 BM8563 恢复; SNTP 同步成功后写回 BM8563
 * RTC 中存本地时间 (TZ 固定 CST-8), 读写两侧一致即可。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 设 TZ + 系统时间无效时从 BM8563 恢复。须在 BSP I2C 初始化后调用。 */
void pvc_clock_init(void);

/* 周期调用 (pvc_power 心跳): SNTP 同步后把系统时间写回 BM8563 (仅一次) */
void pvc_clock_tick(void);

/* 系统时间是否可信 (2024 年之后) */
bool pvc_clock_valid(void);

/* "HH:MM" 本地时间; 无效时给 "--:--" */
void pvc_clock_hhmm(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
