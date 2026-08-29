/*
 * pvc_sdio.h - SD 卡访问互斥 (CoreS3 硬件约束)
 *
 * CoreS3 的 SD(SDSPI) 与 LCD 共享 SPI2 总线。sdspi 的 polling 事务与
 * esp_lcd 刷屏事务并发会触发 spi_hal_setup_trans 断言复位
 * (真机实证: 插卡后拍照/feed 落盘必崩)。
 *
 * 约定: 非 LVGL 任务的一切 SD I/O (fopen/fwrite/opendir/mkdir/remove)
 * 必须包在 pvc_sd_lock/unlock 之间 —— 持显示锁阻止 LVGL 发起刷屏。
 * LVGL 任务自身的 SD 读 (相册) 无需包 (与刷屏同任务天然串行);
 * 锁为递归互斥, LVGL 任务误用也安全。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void pvc_sd_lock(void);      /* 阻塞等待 (显示锁) */
void pvc_sd_unlock(void);

#ifdef __cplusplus
}
#endif
