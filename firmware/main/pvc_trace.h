/*
 * pvc_trace.h - 自动化测试埋点
 *
 * 统一单行格式: "[EV] <event> k=v k=v ...", 由 test/analyze_log.py 解析。
 * 约定:
 *   - event 与 key 用小写下划线; 值不含空格 (字符串值截断/替换空格)
 *   - 与 [FW] 日志互补: [FW] 是规范约定的联调日志 (请求/SLEEP/PROV),
 *     [EV] 覆盖状态机与业务事件; 分析工具两者都解析
 * 埋点清单 (analyze_log.py 的检查项依赖这些事件名, 改名需同步):
 *   wifi_up / wifi_down / net(state=) / pair_code / pair_bound / pair_fail
 *   photo_captured / photo_encoded / upload_queued|sent|drop|defer
 *   feed_poll / feed_err / react_send / config_recv / config_ack
 *   stat (30s 心跳: heap/min_heap/queue/state)
 */
#pragma once

#include <stdio.h>

#define PVC_EV(fmt, ...) printf("[EV] " fmt "\n", ##__VA_ARGS__)
