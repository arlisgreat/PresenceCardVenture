/*
 * pvc_power.h - 省电管理 (规范 §6 "省电 (电池设备)")
 *
 *  - 无操作 60s -> deep sleep (相机关闭 + 灭屏)
 *  - 定时 5 分钟唤醒 "静默轮询": 不亮屏, 等联网任务跑完
 *    state -> feed -> 补传 (目标 < 20s 在线) 后回睡
 *  - 触摸唤醒 / 静默期间触摸 -> 亮屏恢复正常交互
 *  - 配网 / 配对状态不休眠 (用户正在操作)
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 启动省电任务。quiet_boot = 本次是定时器唤醒 (静默轮询模式):
 * main 不亮屏不开相机, 由本模块在用户触摸时补开。
 */
void pvc_power_init(bool quiet_boot);

#ifdef __cplusplus
}
#endif
