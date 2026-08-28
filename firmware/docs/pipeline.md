# 预览与拍照管线

> 对应代码:`main/ui_beauty_camera.c`(管线编排)、`main/app_camera.c`(相机)、
> `main/hw2d.c` + `main/hw2d_pie.S`(像素算子)。埋点名与检查项见
> `main/pvc_trace.h` / `test/analyze_log.py`。

## 硬件事实(设计前提)

- ESP32-S3 **没有 JPEG 硬件编解码器,也没有 2D 像素加速器**(均为 P4 才有);
  GC0308 传感器**不能直出 JPEG**。因此 JPEG 编解码全为软件(jpge / TJpgDec),
  "硬件加速"指:相机 I2S DMA、LCD SPI DMA、PIE 128-bit SIMD、双核并行。
- PSRAM(QSPI 8MB)是共享瓶颈:相机 DMA 写、渲染读写、LCD DMA 读同时压在上面。

## 预览管线(core 0,LVGL 任务,40ms 定时器 = 25fps 上限)

```
GC0308 ──DVP──> I2S 并行 DMA ──> PSRAM 双缓冲 fb[0]/fb[1]     (自由运行, 零 CPU)
                                      │
每 40ms preview_timer_cb (LVGL 任务, 持显示锁):
  1. app_camera_grab()        取最新帧(GRAB_LATEST), 归还上一帧;
                              相机未初始化(静默唤醒)时快速返回 NULL 跳过
  2. hw2d_scale_stat          VGA 640x480 → QVGA 320x240 (Q16 双线性,
                              直读 DMA 缓冲零拷贝) → s_preview_qvga
  3. hw2d_filter_lut_stat     LUT 滤镜 → s_canvas_buf
                              (参数变化才重建表; 黑白走灰度专用路径)
  4. blend_circle x3 (可选)   贴纸: hw2d_fill 白源 + SRC_OVER 混合
  5. lv_obj_invalidate        LVGL 渲染 → esp_lvgl_port SPI DMA 异步刷屏
                              (与下一帧计算重叠, 不占 40ms 预算)
  6. 每 8 帧/滤镜切换         update_thumbs: 缩至 22x22 + 6 种滤镜 LUT
  7. app_camera_release()
```

**每帧 CPU 预算**:第 2 步(读 60 万像素,PSRAM 带宽敏感,估 8–15ms)+
第 3 步(标称 3–5ms)+ LVGL 绘制。帧率上限由三者之和能否塞进 40ms 决定。

**观测**:每秒 `[EV] perf_preview fps=/render_avg_us=/cpu_pct=`(红线 C13:
目标 25,均值 <20 WARN、<10 FAIL);每 30s `[hw2d]` 算子级均值。

**降级**:相机失败/静默唤醒未开相机 → grab 返 NULL,UI 正常画面静止;
相机降级 QVGA 时跳过缩放直接渲染。

## 拍照管线(两段式:快门轻、后处理重)

### 第一段:快门(core 0,LVGL 任务,阻塞 UI 仅 ~50–80ms)

```
take_photo():
  1. 闪白: hw2d_fill 全屏白 + lv_refr_now (同步刷一帧作快门反馈)
  2. PSRAM 分配 snap (VGA 600KB)
  3. grab → hw2d_copy 整帧拷出 → release          [grab_ms 埋点]
  4. 组装 photo_job: snap + 尺寸 + 滤镜参数按值快照(含美白 bias)
     + 磨皮/美白值 + 滤镜 API id + seq + 时间戳
  5. xQueueSend(深度 2) ─成功→ 返回, 预览立即恢复
                        └队满→ 释放 snap + toast + [EV] photo_drop (C16)
```

参数**按值快照**:入队后切滤镜/调滑杆不影响已拍照片。

### 第二段:后处理(core 1,photo_worker,与预览并行)

```
photo_worker 出队 (queue_ms 埋点):
  1. 磨皮   hw2d_blur3x3 (平面拆分+3x3 滑窗+查表除法);
            smooth=0 则 memcpy; OOM 降级存原图
  2. 滤镜   hw2d_apply_filter_exact (逐像素精确, 用快照参数)
  3. 相册   save_jpg: hw2d_swap16(PIE) → fmt2jpg q90
            → /sdcard/DCIM/img_<boot>_<seq>.jpg   (boot 前缀防重启覆盖)
            [EV] photo_captured
  4. 上传编码 hw2d_scale_be (VGA→QVGA, 缩放+大端输出融合单遍, 无独立 swap)
            → fmt2jpg 质量链 90→80→60 (超 100KB 才降档)   [EV] perf_encode
  5. 释放 snap; 成功 → [EV] photo_encoded → pvc_net_enqueue_photo
            → 上传队列 (SD /sdcard/queue 断电补传; 无 SD 降级 PSRAM)
            → 事件位唤醒 net 任务
  6. [EV] perf_photo grab/queue/blur/filter/save/total; 锁内 toast 文件名
                                      │
net 任务 (core 0) 唤醒 → drain → POST /photos
  (Idempotency-Key / X-Device-Id / X-Filter-Id / X-Beauty)
  → [FW] POST /photos 201 → [EV] upload_sent → web 可见
```

**耗时预估**(快门→入队,C14 阈值 warn>3s):磨皮 100–300ms + 滤镜 50–150ms
+ q90 VGA 编码写卡 300–500ms + 缩放 ~20ms + QVGA 编码 150–400ms ≈ **1–1.5s**,
全程不阻塞预览。持续连拍能力 ≈ 每 1.5s 一张(队列深 2 刻意限流,防 PSRAM 峰值)。

## 并发与资源规则

| 维度 | 预览 (core 0) | 拍照后处理 (core 1) |
|---|---|---|
| hw2d 算子 | `_stat` 带统计版 | 无统计版(计数器跨核累加会竞争) |
| PIE swap16 | 相册/feed 解码 | save_jpg;**try-lock 互斥**单任务使用,失锁走 C 标量 |
| 共享缓冲 | `s_preview_qvga` 仅预览 | worker 自备 snap/work/be |
| PSRAM 峰值 | 常驻 ~0.9MB | 2 并发 job ≈2.8MB(8MB 内) |
| SD/FATFS | 相册读(LVGL 任务) | 相册写+队列写;FATFS 可重入锁 |
| 相机 | grab/release 仅 LVGL 任务 | 不碰相机(快门段已拷出) |

## 已知取舍与真机验证点

1. 快门抓"闪白瞬间的最新帧":闪白仅在屏幕,不影响相机曝光;grab 最坏多等
   一个帧周期(~33–50ms)。
2. 队列深 2 的连拍限流是刻意设计;`photo_drop`/C16 统计被拒次数。
3. 编码期(core 1)与预览(core 0)并行压 PSRAM 总线,拍后 ~1.5s 窗口内预览
   帧率可能小幅下降 —— `perf_preview` 样本量化,明显掉帧再给 worker 节流。
4. 两处字节序假设(上传 `scale_be`、显示 feed swap):host 单测已证数学等价,
   方向正确性以真机颜色为准;PIE 汇编由开机自测背书(`[EV] simd pie_swap=1`)。
