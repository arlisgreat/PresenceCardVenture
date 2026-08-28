/*
 * ui_beauty_camera.h - LVGL 美颜相机 UI
 *
 * 功能: 实时预览 / 滤镜(6种) / 美颜(美白+磨皮) / 贴纸面板 / 拍照(BMP) / 相册
 * 帧管线: 由内部 LVGL 定时器主动 app_camera_grab() -> 渲染 -> release (零拷贝)
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建美颜相机 UI（需在 LVGL 初始化后、app_camera_init() 前调用）
 *        内部注册一个 25FPS LVGL 定时器，每次 tick 抓取最新摄像头帧渲染预览
 */
void ui_beauty_camera_create(void);

/*
 * 联网层 UI 桥 (pvc_net 回调用, 内部自行 bsp_display_lock, 任意任务可调):
 *   - 配对码全屏覆盖层 (规范 §1: 屏幕显示 6 位配对码)
 *   - 状态栏右侧联网状态短文案
 */
void ui_net_show_pair(const char *code);
void ui_net_hide_pair(void);
void ui_net_show_prov(const char *ble_name, const char *pop);  /* BLE 配网引导页 */
void ui_net_set_status(const char *txt);
void ui_net_feed_updated(int total, int fresh, int new_likes);  /* feed 更新/被赞 */

/* web 下发配置应用 (pvc_config 回调; 内部加显示锁) */
#include "pvc_config.h"
void ui_apply_remote_config(const pvc_config_t *cfg);

#ifdef __cplusplus
}
#endif
