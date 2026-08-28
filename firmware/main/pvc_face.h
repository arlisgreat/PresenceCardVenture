/*
 * pvc_face.h - 人脸检测 (esp-dl human_face_detect), 贴纸跟脸用
 *
 * 架构: 预览每 ~320ms 提交一帧 QVGA 副本 -> core1 低优先级检测任务
 * (50-150ms/帧) -> 最新人脸框缓存; 预览渲染按最近结果放置贴纸,
 * 结果超过 2s 视为无脸回落固定位置。检测仅在贴纸开启时进行。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    int  x, y, w, h;      /* QVGA (320x240) 坐标系下的人脸框 */
} pvc_face_box_t;

/* 加载模型 + 启动检测任务 (失败返回 false, 贴纸回落固定位置) */
bool pvc_face_init(void);

/* 提交一帧 QVGA RGB565(LE) 检测 (内部拷贝; 检测中则丢弃本帧, 非阻塞) */
void pvc_face_submit(const uint16_t *qvga);

/* 取最近人脸框; 无有效结果 (含超时 2s) 返回 false */
bool pvc_face_latest(pvc_face_box_t *out);

#ifdef __cplusplus
}
#endif
