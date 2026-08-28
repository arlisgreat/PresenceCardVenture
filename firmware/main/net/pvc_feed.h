/*
 * pvc_feed.h - 好友动态拉取与本地缓存 (规范 §3)
 *
 *  - §3.1 GET /device/state: unseen_count > 0 才拉 feed (省电省流量)
 *  - §3.2 GET /feed?limit=8 + If-None-Match etag; 304 无更新
 *  - §3.3 下载 JPEG 缓存最近 8 张 (PSRAM 槽位 + SD /sdcard/feed 镜像, 离线可翻看)
 *  - §3.4 反应 (点赞) 走异步队列, 由联网任务发送
 *
 * 线程模型: poll/process 仅联网任务调用; snapshot/read/react 任意任务 (内部互斥)。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PVC_FEED_MAX 8

typedef struct {
    char photo_id[48];
    char author[24];       /* display_name (UTF-8, 截断安全) */
    char caption[96];
    char filter[16];
} pvc_feed_item_t;

/* poll 返回值: >=0 = 本次新增张数; 负值为错误 */
#define PVC_FEED_ERR   (-1)
#define PVC_FEED_AUTH  (-2)   /* 401: 调用方清 token 回配对 */

/* 启动时调用 (联网任务): 从 SD 镜像恢复缓存, 离线也有内容可看 */
void pvc_feed_init(void);

/* 联网任务调用: state -> feed -> 下载新图 -> 更新缓存与 SD 镜像 */
int pvc_feed_poll(void);

/* 缓存条目数 / 元数据快照 (拷贝, 无指针共享) */
int pvc_feed_snapshot(pvc_feed_item_t *out, int max);

/* 按 photo_id 拷出 JPEG; 返回字节数, 未命中/缓冲不足返回 -1 */
int pvc_feed_read_jpeg(const char *photo_id, uint8_t *buf, size_t cap);

/* UI 调用: 点赞入异步队列 (type: "heart"/"thumbsup"/"wow", §3.4) */
void pvc_feed_react_async(const char *photo_id, const char *type);

/* 联网任务调用: 发送积压的反应; 返回 PVC_FEED_AUTH 表示 401 */
int pvc_feed_process_reactions(void);

#ifdef __cplusplus
}
#endif
