/*
 * pvc_upload.h - 拍照上传待传队列 (规范 §2 + §6)
 *
 *  - Idempotency-Key = "{device_id}-{boot计数}-{照片序号}", 重试恒同键
 *  - 失败退避 1s -> 4s -> 15s; 仍失败留在队列, 下轮排空重试
 *  - 有 SD 卡: 落盘 /sdcard/queue/<boot>_<seq>_<filter>.jpg, 断电/重启可补传
 *    无 SD 卡: 驻留 PSRAM (重启丢失, 联网即传)
 *  - 409 视为成功 (幂等); 400/413/415 属固件 bug, 丢弃该照片防堵塞队列
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PVC_UP_ALL_SENT = 0,   /* 队列已清空 */
    PVC_UP_RETRY_LATER,    /* 有条目暂时失败, 稍后再排空 */
    PVC_UP_AUTH_FAIL,      /* 401: 调用方需清 token 回配对 */
} pvc_up_result_t;

/* 启动时调用: 扫描 /sdcard/queue 遗留文件重建队列 (无 SD 时静默跳过) */
void pvc_upload_init(void);

/* 照片入队 (线程安全)。jpg 内容会被复制/落盘, 调用后可释放。 */
esp_err_t pvc_upload_enqueue(const uint8_t *jpg, size_t len, const char *filter_id);

/* 排空队列 (仅在联网任务中调用), 内部按条目做退避重试 */
pvc_up_result_t pvc_upload_drain(void);

int pvc_upload_depth(void);

#ifdef __cplusplus
}
#endif
