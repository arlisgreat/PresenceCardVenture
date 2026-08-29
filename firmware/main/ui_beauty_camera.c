/*
 * ui_beauty_camera.c - LVGL 美颜相机 UI (LVGL 9.5, M5Stack CoreS3 320x240)
 *
 * 基于 tools/ui_design 设计稿重构, 并接入 hw2d 硬件 2D 加速层:
 *
 *   ┌──────────────────────────────────┐
 *   │ 09:41      美颜·白皙       100%  │  <- 状态栏 20px
 *   ├──────────────────────────────────┤
 *   │  [缩略]                          │
 *   │  [缩略]  ┌──────────────┐ 美白80 │  <- 全屏预览 320x240
 *   │  [缩略]  │  实时预览     │ 磨皮60 │
 *   │  [缩略]  │ (canvas 全屏) │        │
 *   │  [缩略]  └──────────────┘        │
 *   │  [缩略]                          │
 *   ├──────────────────────────────────┤
 *   │ 相册    (快门)    贴纸    翻转    │  <- 工具栏 40px
 *   └──────────────────────────────────┘
 *
 * 硬件 2D 加速 (见 hw2d.h):
 *   1. esp_camera I2S DMA 零拷贝: UI 定时器 grab 最新帧, 渲染直读 PSRAM 缓冲
 *   2. hw2d_apply_filter_lut : 预览滤镜 (LUT 查表, ~3-5ms/帧)
 *   3. hw2d_blur3x3         : 拍照磨皮 (平面法+查表除法)
 *   4. hw2d_apply_filter_exact : 拍照滤镜 (逐像素精确)
 *   5. hw2d_alpha_blend     : 贴纸合成 (SRC_OVER)
 *   6. hw2d_fill            : 拍照闪光 (64bit 宽写)
 *   7. hw2d_scale           : 相册缩略图 / 滤镜缩略图 / 全屏查看
 *
 * 注意: 所有 LVGL 调用均发生在 LVGL 任务上下文 (LVGL 线程安全)。
 */
#include "ui_beauty_camera.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "bsp/m5stack_core_s3.h"

#include "hw2d.h"
#include "app_camera.h"
#include "pvc_net.h"
#include "pvc_feed.h"
#include "pvc_store.h"      /* boot 计数: 相册文件名跨重启唯一 */
#include "pvc_clock.h"
#include "pvc_jpeg.h"
#include "ccd_assets/ccd_luts.h"
#include "pvc_sound.h"
#include "pvc_face.h"
#include "pvc_sdio.h"
#include "pvc_trace.h"
#include "esp_camera.h"
#include "img_converters.h"     /* fmt2jpg / jpg2rgb565 (esp32-camera) */

static const char *TAG = "ui_camera";

#define UI_W 320
#define UI_H 240
#define STATUS_H 24
#define BAR_H 52
#define FRAME_BYTES (UI_W * UI_H * 2)      /* RGB565 全屏 */

/* 系统统一 QVGA 320x240 (产品只要求 QVGA JPEG): 采集=预览=拍照, 零缩放 */
#define CAP_W UI_W
#define CAP_H UI_H
#define CAP_BYTES (CAP_W * CAP_H * 2)      /* QVGA 单帧 RGB565 (150KB, PSRAM) */

#define THUMB_SZ 22                        /* 滤镜缩略图尺寸 */

#define MAX_ALBUM_ITEMS 6

/* 内存分配: 统一 PSRAM */
#define PSRAM_MALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define PSRAM_FREE(p)    heap_caps_free(p)

/* 前置声明 (C 语言无 lambda, 静态函数需在使用前声明) */
static void update_thumbs(const uint8_t *src);
static void update_status(void);
static void close_panel(void);
static void panel_done_cb(lv_event_t *e);
static void style_tool_btn(lv_obj_t *btn);
static void open_filter_panel(void);
static void on_net_status_click(lv_event_t *e);
static void open_feed(void);
static void render_feed(void);
static void do_like(void);
static void spawn_like_float(int count);
static void open_caption_panel(void);
static lv_obj_t *panel_create(uint32_t h);

/* ================= 状态 ================= */
static lv_obj_t   *s_canvas;               /* 全屏预览画布 */
static uint16_t   *s_canvas_buf;           /* canvas 缓冲 (PSRAM) */

/* 渲染节流: 每 ~8 帧 (320ms) 刷新一次滤镜缩略图 */
static uint32_t s_frame_cnt = 0;

/* 滤镜 / 美颜 / 贴纸。扩展索引: 0..5 基础(hw2d 参数式), 6..9 CCD 机型(3D LUT) */
#define FILTER_CCD_BASE  HW2D_FILTER_MAX
#define FILTER_EXT_COUNT (HW2D_FILTER_MAX + CCD_CAM_COUNT)
static int s_filter = 0;
static int s_white = 40;                   /* 美白 0-100 */
static int s_smooth = 40;                  /* 磨皮 0-100 (拍照时) */
static int s_sticker = -1;                 /* -1 = 无贴纸 */
static bool s_mirror = true;               /* 自拍默认镜像; 可在工具栏切换 */

/* 当前生效滤镜参数 (基础滤镜 + 美白叠加); hw2d 按参数内容判断 LUT 重建 */
static hw2d_filter_t s_active_filter;
static hw2d_yuv_luts_t s_yuv_luts;               /* 预览 YUV 表 (core0 专用) */
static hw2d_yuv_luts_t s_thumb_luts[HW2D_FILTER_MAX];  /* 滤镜条 6 组 */
static hw2d_yuv_luts_t s_id_luts;                /* 恒等表 (人脸帧转换用) */
static uint16_t *s_face_rgb;                     /* 人脸检测 RGB565 帧 */

/* 滤镜缩略图 (22x22, 6 个, 与面板共享) */
static uint16_t *s_thumb_raw;              /* 缩放原图 22x22 */
static uint16_t *s_thumb_bufs[HW2D_FILTER_MAX];
static lv_obj_t *s_thumb_canvases[HW2D_FILTER_MAX];
static bool s_thumb_dirty = false;

/* 控件 */
static lv_obj_t *s_status_l, *s_status_c, *s_status_r;   /* 状态栏三区 */
static lv_obj_t *s_mirror_label;
static lv_obj_t *s_panel = NULL;           /* 当前弹出面板 */
static lv_obj_t *s_toast = NULL;
static lv_timer_t *s_toast_timer = NULL;

/* ================= 小工具 ================= */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void toast_auto_del_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    if (s_toast) {
        lv_obj_del(s_toast);
        s_toast = NULL;
    }
    s_toast_timer = NULL;
}

static void toast_show(const char *msg)
{
    if (s_toast) {
        lv_obj_del(s_toast);
        s_toast = NULL;
    }
    if (s_toast_timer) {
        lv_timer_del(s_toast_timer);
        s_toast_timer = NULL;
    }
    s_toast = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_toast, 200, 28);
    lv_obj_align(s_toast, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(s_toast, lv_color_make(0x00, 0x00, 0x00), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_toast, 14, 0);
    lv_obj_set_style_border_width(s_toast, 0, 0);
    lv_obj_set_style_shadow_width(s_toast, 0, 0);
    lv_obj_t *l = lv_label_create(s_toast);
    lv_label_set_text(l, msg);
    lv_obj_center(l);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    s_toast_timer = lv_timer_create(toast_auto_del_cb, 1800, NULL);
    /* LVGL 9.x 已移除 lv_timer_set_auto_reload: 回调内 lv_timer_del(t) 自删, 天然一次性 */
}

/* ================= 滤镜参数 ================= */
static void update_active_filter(void)
{
    const hw2d_filter_t *b = (s_filter >= FILTER_CCD_BASE)
        ? &k_ccd_cams[s_filter - FILTER_CCD_BASE].preview
        : hw2d_filter_get((hw2d_filter_id_t)s_filter);
    /* 美白: 亮度偏置 + 微降饱和 */
    s_active_filter = *b;
    s_active_filter.bias = (int8_t)((int)b->bias + (s_white * 18) / 100);
    if (s_active_filter.bias > 64) s_active_filter.bias = 64;
    int sat = (int)b->sat - (s_white * 10) / 100;
    s_active_filter.sat = (uint8_t)(sat < 0 ? 0 : sat);
}

/* ================= 预览渲染 (LVGL 任务上下文) ================= */
static uint16_t s_white_line[UI_W]; /* 贴纸混合用白色源 (静态) */

/*
 * 在 canvas 上混合一个白色实心圆 (头顶兔耳/皇冠占位)。
 * 使用 hw2d_fill 生成源 + hw2d_alpha_blend SRC_OVER 混合, 展示硬件加速链路。
 */
static void blend_circle(uint16_t *canvas, int cx, int cy, int r, uint8_t alpha)
{
    hw2d_fill(s_white_line, UI_W, 0xFFFF);
    if (cx - r < 0 || cx + r >= UI_W) return;   /* 跟脸坐标可能越界: 整圆裁剪 */
    for (int y = cy - r; y <= cy + r; y++) {
        int x0, x1, x;
        if (y < 0 || y >= UI_H) continue;
        x0 = x1 = cx;
        for (x = cx + 1; x <= cx + r; x++) {
            int ddx = x - cx, ddy = y - cy;
            if (ddx * ddx + ddy * ddy <= r * r) x1 = x; else break;
        }
        for (x = cx - 1; x >= cx - r; x--) {
            int ddx = x - cx, ddy = y - cy;
            if (ddx * ddx + ddy * ddy <= r * r) x0 = x; else break;
        }
        if (x0 <= x1) {
            hw2d_blend_stat(&canvas[y * UI_W + x0], &s_white_line[x0],
                            (uint32_t)(x1 - x0 + 1), alpha);
        }
    }
}

/* 贴纸与缩略图刷新 (canvas 已含滤镜后画面; thumb_src 为未滤镜 YUYV 或 NULL) */
static void render_overlays(const uint8_t *thumb_src)
{
    /* 贴纸 (SRC_OVER 硬件混合): 有人脸框则跟脸放置, 否则固定位置 */
    if (s_sticker >= 0 && s_sticker < 6) {
        pvc_face_box_t fb;
        if (pvc_face_latest(&fb)) {
            int r = fb.w / 6;
            if (r < 8) r = 8;
            if (r > 32) r = 32;
            int ey = fb.y - fb.h / 10;
            blend_circle(s_canvas_buf, fb.x + fb.w / 4, ey, r, 170);
            blend_circle(s_canvas_buf, fb.x + (fb.w * 3) / 4, ey, r, 170);
            blend_circle(s_canvas_buf, fb.x + fb.w / 2, fb.y - fb.h / 6,
                         (r * 2) / 3, 200);
        } else {
            blend_circle(s_canvas_buf, 84, 30, 21, 170);
            blend_circle(s_canvas_buf, 236, 30, 21, 170);
            blend_circle(s_canvas_buf, 160, 26, 14, 200); /* 头顶装饰 */
        }
    }
    lv_obj_invalidate(s_canvas);
    if (thumb_src) update_thumbs(thumb_src);
}

static void preview_timer_cb(lv_timer_t *t)
{
    (void)t;
    const app_camera_frame_t *f;
    if (!s_canvas) return;
    f = app_camera_grab();
    if (!f) return;

    /* 性能监控: 统计每帧渲染总耗时 (验证 hw2d 硬件加速预算) */
    static uint32_t perf_frames = 0, perf_rend_us = 0, perf_last_ms = 0;
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    bool thumb_due = s_thumb_dirty || ((++s_frame_cnt & 7u) == 0);
    if (f->width == UI_W && f->height == UI_H) {
        /* QVGA YUV422 直通: YUV 域滤镜 + RGB565 转换融合单遍 (显示边界) */
        const uint8_t *yuyv = (const uint8_t *)f->buf;
        hw2d_yuv_build_luts(&s_active_filter, &s_yuv_luts);
        /* 终极融合单遍: 装配 180 度 + 镜像 + 色度降噪 + 滤镜 + RGB565
         * (真机: rot180+hmirror+smooth 三遍叠加曾达 60ms/帧; 镜像时
         * rot180∘hmirror = 纯垂直翻转, 全顺序访存, 不再改写相机帧) */
        hw2d_yuv_render_rgb565_stat(s_canvas_buf, yuyv, UI_W, UI_H,
                                    &s_yuv_luts, s_mirror);
        if (thumb_due) {
            if (s_sticker >= 0 && s_face_rgb) {
                /* 人脸检测吃 RGB565: 恒等表 + 同几何变换 (每 ~320ms),
                 * 人脸框坐标与画面/照片一致 */
                hw2d_yuv_render_rgb565(s_face_rgb, yuyv, UI_W, UI_H,
                                       &s_id_luts, s_mirror);
                pvc_face_submit(s_face_rgb);
            }
            s_thumb_dirty = false;
            render_overlays(yuyv);         /* 缩略图用未滤镜 YUYV 原帧 */
        } else {
            render_overlays(NULL);
        }
    } else {
        static bool s_warned;
        if (!s_warned) {
            s_warned = true;
            ESP_LOGE(TAG, "unexpected frame %ux%u (expect QVGA)",
                     (unsigned)f->width, (unsigned)f->height);
        }
    }
    perf_frames++;
    perf_rend_us += (uint32_t)esp_timer_get_time() - t0;
    app_camera_release();

    /* 每秒输出一次: FPS / 平均渲染耗时 / 渲染 CPU 占比 / 剩余堆
     * (perf_preview 供 analyze_log.py 判帧率红线: 目标 25, 低于阈值报警) */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - perf_last_ms >= 1000) {
        PVC_EV("perf_preview fps=%lu render_avg_us=%lu cpu_pct=%lu heap=%lu",
               (unsigned long)perf_frames,
               (unsigned long)(perf_frames ? perf_rend_us / perf_frames : 0),
               (unsigned long)(perf_rend_us / 10000), /* us/秒 -> % (1e6us=100%) */
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
        static uint32_t perf_sec = 0;
        if ((++perf_sec % 30) == 0) { /* 每 30s 输出一次各算子平均耗时 */
            hw2d_stats_dump();
            hw2d_stats_reset();
        }
        perf_last_ms = now_ms;
        perf_frames = 0;
        perf_rend_us = 0;
    }
}

/* ================= 滤镜缩略图 (22x22) ================= */
static void update_thumbs(const uint8_t *src)
{
    if (!s_thumb_raw || !s_thumb_bufs[0]) return;

    /* 点采样 YUYV 到 22x22 小帧 (对为单位取样, 保持 YCbYCr 结构) */
    uint8_t *t = (uint8_t *)s_thumb_raw;          /* 复用: 22*22*2 字节恰好 */
    for (int y = 0; y < THUMB_SZ; y++) {
        uint32_t sy = (uint32_t)y * UI_H / THUMB_SZ;
        const uint8_t *row = src + (size_t)sy * UI_W * 2;
        for (int x = 0; x < THUMB_SZ; x += 2) {
            uint32_t sx = ((uint32_t)x * UI_W / THUMB_SZ) & ~1u;
            const uint8_t *pp = row + sx * 2;
            uint8_t *op = t + ((size_t)y * THUMB_SZ + (size_t)x) * 2;
            op[0] = pp[0]; op[1] = pp[1]; op[2] = pp[2]; op[3] = pp[3];
        }
    }
    for (int i = 0; i < HW2D_FILTER_MAX; i++) {
        hw2d_yuv_build_luts(hw2d_filter_get((hw2d_filter_id_t)i),
                            &s_thumb_luts[i]);
        hw2d_yuv_filter_rgb565_rot180(s_thumb_bufs[i], t, THUMB_SZ * THUMB_SZ,
                                      &s_thumb_luts[i]);
        if (s_mirror) hw2d_rgb565_hmirror(s_thumb_bufs[i], THUMB_SZ, THUMB_SZ);
        if (s_thumb_canvases[i]) lv_obj_invalidate(s_thumb_canvases[i]);
    }
}

/* ================= 相册 JPEG 存取 ================= */

/* 编码产物落盘 (编码本身在 worker 用 pvc_jpeg + 复用缓冲完成) */
static bool write_file(const char *path, const uint8_t *data, size_t len)
{
    /* 临时文件 + 改名: 崩溃/掉电打断 fwrite 不会留下残缺正式文件
     * (残缺 JPEG 解码出绿色块; 恢复侧另有 pvc_jpeg_intact 双保险) */
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "wb");
    bool ok = fp && fwrite(data, 1, len, fp) == len;
    if (fp) fclose(fp);
    if (ok) {
        remove(path);
        ok = (rename(tmp, path) == 0);
    } else {
        remove(tmp);
    }
    if (!ok) ESP_LOGE(TAG, "write %s failed", path);
    return ok;
}

/*
 * JPEG 文件 -> RGB565 (小端)。利用 TJpgDec 的 1/2、1/4、1/8 降尺度解码:
 * 选能放进 maxw x maxh 的最小缩放档, 缩略图无需全尺寸解码 (快 4-10 倍)。
 * out 容量须 >= maxw x maxh x 2 字节; 实际尺寸经 ow/oh 出参返回。
 */
/* 内存版: JPEG 缓冲 -> RGB565 (相册 PSRAM 兜底与文件路径共用) */
static int decode_jpg_565(const uint8_t *jbuf, size_t sz, uint16_t *out,
                          uint32_t maxw, uint32_t maxh,
                          uint32_t *ow, uint32_t *oh)
{
    uint32_t w = 0, h = 0;
    int s;
    if (!pvc_jpeg_dims(jbuf, sz, &w, &h)) return -1;
    for (s = 0; s <= 3; s++) {
        if ((w >> s) <= maxw && (h >> s) <= maxh) break;
    }
    if (s > 3) return -1;

    /* jpg2rgb565 输出小端 RGB565: 直接解码进目标缓冲, 无需中转/交换 */
    if (!jpg2rgb565(jbuf, sz, (uint8_t *)out, (esp_jpeg_image_scale_t)s)) {
        return -1;
    }
    *ow = w >> s;
    *oh = h >> s;
    return 0;
}

static int load_jpg_565(const char *path, uint16_t *out, uint32_t maxw,
                        uint32_t maxh, uint32_t *ow, uint32_t *oh)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) { fclose(f); return -1; }
    uint8_t *jbuf = PSRAM_MALLOC((size_t)sz);
    if (!jbuf || fread(jbuf, 1, (size_t)sz, f) != (size_t)sz) {
        if (jbuf) PSRAM_FREE(jbuf);
        fclose(f);
        return -1;
    }
    fclose(f);

    /* 完整性快检: 残缺文件 (掉电打断写入) 解码出绿色块, 直接判失败 */
    int rc = pvc_jpeg_intact(jbuf, (size_t)sz)
        ? decode_jpg_565(jbuf, (size_t)sz, out, maxw, maxh, ow, oh) : -1;
    PSRAM_FREE(jbuf);
    return rc;
}

/* ---------------- 无 SD 卡相册兜底: 最近照片 JPEG 存 PSRAM 环 ----------------
 * worker (core1) 写入 / LVGL 任务读取: 互斥锁保护指针与解码期间的缓冲生命期 */
static uint8_t *s_ram_jpg[MAX_ALBUM_ITEMS];
static size_t   s_ram_len[MAX_ALBUM_ITEMS];
static int s_ram_head, s_ram_cnt;          /* head = 下一写位 */
static SemaphoreHandle_t s_ram_mtx;

static void ram_album_push(const uint8_t *jpg, size_t len)
{
    if (!s_ram_mtx) s_ram_mtx = xSemaphoreCreateMutex();
    if (!s_ram_mtx) return;
    uint8_t *copy = PSRAM_MALLOC(len);
    if (!copy) return;
    memcpy(copy, jpg, len);
    xSemaphoreTake(s_ram_mtx, portMAX_DELAY);
    if (s_ram_jpg[s_ram_head]) PSRAM_FREE(s_ram_jpg[s_ram_head]);
    s_ram_jpg[s_ram_head] = copy;
    s_ram_len[s_ram_head] = len;
    s_ram_head = (s_ram_head + 1) % MAX_ALBUM_ITEMS;
    if (s_ram_cnt < MAX_ALBUM_ITEMS) s_ram_cnt++;
    xSemaphoreGive(s_ram_mtx);
}

/* 第 i 新的 RAM 照片解码 (i=0 最新); 成功 0 */
static int ram_album_decode(int i, uint16_t *out, uint32_t maxw, uint32_t maxh,
                            uint32_t *ow, uint32_t *oh)
{
    if (!s_ram_mtx) return -1;
    int rc = -1;
    xSemaphoreTake(s_ram_mtx, portMAX_DELAY);
    if (i >= 0 && i < s_ram_cnt) {
        int slot = (s_ram_head - 1 - i + 2 * MAX_ALBUM_ITEMS) % MAX_ALBUM_ITEMS;
        if (s_ram_jpg[slot]) {
            rc = decode_jpg_565(s_ram_jpg[slot], s_ram_len[slot], out,
                                maxw, maxh, ow, oh);
        }
    }
    xSemaphoreGive(s_ram_mtx);
    return rc;
}

/* ================= 上传 (Presence Card 规范 §2) ================= */
/*
 * filter_id 对齐 web/server 清单 (none/warm/bw/film/vivid, 规范 §5):
 *   原图→none  白皙→none(美白走 X-Beauty)  暖阳→warm
 *   冷调→vivid(近似)  黑白→bw  复古→film
 */
static const char *const k_filter_api_id[HW2D_FILTER_MAX] = {
    "none", "none", "warm", "vivid", "bw", "film"
};

static const char *filter_api_id(int idx)
{
    if (idx >= FILTER_CCD_BASE) return k_ccd_cams[idx - FILTER_CCD_BASE].api_id;
    return k_filter_api_id[idx];
}

/* ================= 拍照后处理 worker (core 1) ================= */
/*
 * 快门 (LVGL/core0) 只做抓帧拷贝, 重活 (磨皮/滤镜/q90 存卡/缩放/编码/入队)
 * 全部在 core1 异步执行, 预览不停顿。
 * 并发约定:
 *   - worker 只用无统计版 hw2d 算子 (scale/blur/filter_exact 纯栈可重入;
 *     _stat 计数器与 core0 预览并发累加会竞争, 故不用)
 *   - 参数 (滤镜/磨皮/美白) 在快门时刻按值快照入 job, 与 UI 后续修改解耦
 *   - PIE 每核仅一个使用者 (core0=LVGL, core1=worker), 寄存器组天然隔离
 */
typedef struct {
    uint16_t     *snap;        /* VGA 帧 (缓冲池槽位, worker 归还) */
    int           slot;        /* 池槽位号 */
    uint32_t      w, h;
    hw2d_filter_t filter;      /* 快门时刻的合成滤镜参数 (含美白) */
    int           smooth;
    int           white;
    int           sticker;     /* 贴纸合成进照片 (-1 = 无) */
    pvc_face_box_t fbox;       /* 快门时刻人脸框快照 (无效则固定位) */
    bool          fbox_valid;
    bool          mirror;      /* 快门时刻自拍方向快照 */
    int           fid;       /* 扩展滤镜索引 (含 CCD) */
    uint32_t      seq;
    int64_t       t_shutter;
    int           grab_ms;
} photo_job_t;

static QueueHandle_t s_photo_q;

/* ---- 快照缓冲池: 预分配 3 x 600KB, 消除拍照路径反复 malloc/free ----
 * 3 = 队列深 2 + 处理中 1; 池空即拒拍 (与队满同语义)。
 * acquire 在 LVGL 任务, release 在 worker: 自旋临界区保护位图。 */
#define SNAP_POOL_N 3
static uint16_t *s_snap_pool[SNAP_POOL_N];
static uint8_t   s_snap_used_mask;
static portMUX_TYPE s_pool_mux = portMUX_INITIALIZER_UNLOCKED;

static int snap_acquire(void)
{
    int slot = -1;
    taskENTER_CRITICAL(&s_pool_mux);
    for (int i = 0; i < SNAP_POOL_N; i++) {
        if (s_snap_pool[i] && !(s_snap_used_mask & (1u << i))) {
            s_snap_used_mask |= (1u << i);
            slot = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_pool_mux);
    return slot;
}

static void snap_release(int slot)
{
    taskENTER_CRITICAL(&s_pool_mux);
    s_snap_used_mask &= (uint8_t)~(1u << slot);
    taskEXIT_CRITICAL(&s_pool_mux);
}

/* worker 专属复用缓冲: 编码输出 (YUV 磨皮/滤镜均原地, 无需工作区) */
static uint8_t  *s_wk_enc;                      /* 编码输出 */
#define WK_ENC_CAP (128 * 1024)                 /* QVGA q90 实测 ~30-60KB */

/* ---- 预设配文信箱: 快门后弹 4 短语面板, worker 在上传前最多等 6s ----
 * 连拍时配文归属最近一次快门; 前一张若未及选择则不配文 (刻意从简)。 */
static SemaphoreHandle_t s_caption_sem;
static char s_caption_sel[96];

static void photo_worker(void *arg)
{
    (void)arg;
    photo_job_t job;
    for (;;) {
        if (xQueueReceive(s_photo_q, &job, portMAX_DELAY) != pdTRUE) continue;
        int64_t t_deq = esp_timer_get_time();
        uint32_t pw = job.w, ph = job.h;
        uint32_t pbytes = pw * ph * 2;

        /* YUV 域: Y 平面磨皮 -> Y/Cb/Cr 查表滤镜 (worker 自持表, 与预览无竞争)
         * -> 贴纸合成 (亮度提亮圆, 位置取快门时刻人脸框) */
        uint8_t *yuyv = (uint8_t *)job.snap;
        static hw2d_yuv_luts_t s_wk_luts;      /* worker 单任务专用 */
        /* 装配方向补偿: 先转 180 度, 后续人脸框/贴纸坐标即与预览一致 */
        hw2d_yuv_rot180(yuyv, pw * ph);
        hw2d_yuv_chroma_smooth(yuyv, pw, ph);  /* 成片同预览: 色度降噪 */
        if (job.mirror) hw2d_yuv_hmirror(yuyv, pw, ph);
        hw2d_yuv_blur_y(yuyv, yuyv, pw, ph, (uint8_t)job.smooth);
        int64_t t_blur = esp_timer_get_time();
        if (job.fid >= FILTER_CCD_BASE) {
            /* CCD 机型: 全保真 3D LUT + 颗粒 + 暗角 (预览仅是 1D 近似) */
            const ccd_cam_t *cam = &k_ccd_cams[job.fid - FILTER_CCD_BASE];
            hw2d_yuv_3dlut(yuyv, pw * ph, cam->lut, CCD_LUT_N);
            if (job.white > 0) {           /* 美白叠加: Y 偏置 */
                hw2d_filter_t wf = *hw2d_filter_get(HW2D_FILTER_ORIGINAL);
                wf.bias = (int8_t)((job.white * 18) / 100);
                hw2d_yuv_build_luts(&wf, &s_wk_luts);
                hw2d_yuv_filter(yuyv, yuyv, pw * ph, &s_wk_luts);
            }
            hw2d_yuv_grain(yuyv, pw, ph, cam->grain, cam->grain_hl, job.seq + 7);
            hw2d_yuv_vignette(yuyv, pw, ph, cam->vignette);
        } else {
            hw2d_yuv_build_luts(&job.filter, &s_wk_luts);
            hw2d_yuv_filter(yuyv, yuyv, pw * ph, &s_wk_luts);
        }
        if (job.sticker >= 0) {                /* 贴纸进照片 (与预览同几何) */
            int bx = 60, by = 40, bw2 = 200, bh = 120, rr = 21;
            if (job.fbox_valid) {
                bx = job.fbox.x; by = job.fbox.y;
                bw2 = job.fbox.w; bh = job.fbox.h;
                rr = bw2 / 6;
                if (rr < 8) rr = 8;
                if (rr > 32) rr = 32;
            }
            int ey = by - bh / 10;
            hw2d_yuv_blend_circle(yuyv, pw, ph, bx + bw2 / 4, ey, rr, 170);
            hw2d_yuv_blend_circle(yuyv, pw, ph, bx + (bw2 * 3) / 4, ey, rr, 170);
            hw2d_yuv_blend_circle(yuyv, pw, ph, bx + bw2 / 2, by - bh / 6,
                                  (rr * 2) / 3, 200);
        }
        int64_t t_filter = esp_timer_get_time();

        /* 相册: VGA q90 (esp_new_jpeg, RGB565 LE 直入无字节交换)。
         * 文件名带 boot 计数, 跨重启不覆盖。 */
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/DCIM/img_%05lu_%03lu.jpg",
                 (unsigned long)pvc_store_boot_count(), (unsigned long)job.seq);
        size_t alen = 0;
        if (s_wk_enc) {
            /* QVGA 统一后相册与上传是同一份 q90 编码, 一次编码两用 */
            alen = pvc_jpeg_encode_yuv422(yuyv, pw, ph, 90, s_wk_enc, WK_ENC_CAP);
            if (alen) {
                /* SD 与 LCD 共享 SPI2: worker 写卡须持显示锁 (pvc_sdio.h) */
                pvc_sd_lock();
                mkdir("/sdcard/DCIM", 0755);
                bool saved = write_file(path, s_wk_enc, alen);
                pvc_sd_unlock();
                if (!saved) {
                    /* 无 SD 卡: 相册兜底进 PSRAM 环 (最近 6 张) */
                    ram_album_push(s_wk_enc, alen);
                }
            }
        }
        int64_t t_save = esp_timer_get_time();
        PVC_EV("photo_captured w=%lu h=%lu bytes=%u file=%s",
               (unsigned long)pw, (unsigned long)ph, (unsigned)alen,
               path + sizeof("/sdcard/DCIM/") - 1);

        /* 上传: 帧已是 QVGA, q90 直接编码 (超 100KB 逐档降质), 零缩放 */
        size_t jlen = 0;
        int encode_ms = 0;
        if (s_wk_enc) {
            int64_t t_e0 = esp_timer_get_time();
            if (alen && alen <= 100 * 1024) {
                jlen = alen;               /* 复用相册 q90 编码产物 */
            } else {
                static const uint8_t k_qualities[] = { 80, 60 };
                for (unsigned qi = 0; qi < sizeof(k_qualities); qi++) {
                    jlen = pvc_jpeg_encode_yuv422(yuyv, pw, ph, k_qualities[qi],
                                                  s_wk_enc, WK_ENC_CAP);
                    if (jlen && jlen <= 100 * 1024) break;
                }
            }
            encode_ms = (int)((esp_timer_get_time() - t_e0) / 1000);
        }
        snap_release(job.slot);        /* 帧此后不再使用, 尽早归还池 */
        PVC_EV("perf_encode scale_ms=0 encode_ms=%d bytes=%u lib=espjpeg",
               encode_ms, (unsigned)jlen);

        if (jlen && jlen <= 100 * 1024) {
            /* 等预设配文 (至快门后 6s 截止; 未选/超时 = 不配文) */
            char caption[96] = "";
            int wait_ms = 6000 -
                (int)((esp_timer_get_time() - job.t_shutter) / 1000);
            if (s_caption_sem && wait_ms > 0 &&
                xSemaphoreTake(s_caption_sem, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
                strncpy(caption, s_caption_sel, sizeof(caption) - 1);
            }
            PVC_EV("caption used=%d", (int)(caption[0] != 0));
            PVC_EV("photo_encoded bytes=%u filter=%s beauty=%d",
                   (unsigned)jlen, filter_api_id(job.fid), job.white);
            pvc_net_enqueue_photo(s_wk_enc, jlen, filter_api_id(job.fid),
                                  job.white, caption);  /* enqueue 内部拷贝 */
        } else {
            ESP_LOGE(TAG, "jpeg encode failed or oversize");
        }

        PVC_EV("perf_photo core=1 grab_ms=%d queue_ms=%d blur_ms=%d filter_ms=%d "
               "save_ms=%d total_ms=%d",
               job.grab_ms,
               (int)((t_deq - job.t_shutter) / 1000) - job.grab_ms,
               (int)((t_blur - t_deq) / 1000),
               (int)((t_filter - t_blur) / 1000),
               (int)((t_save - t_filter) / 1000),
               (int)((esp_timer_get_time() - job.t_shutter) / 1000));

        if (bsp_display_lock(500)) {
            toast_show(path + strlen("/sdcard/DCIM/"));
            bsp_display_unlock();
        }
        static uint32_t photo_cnt = 0;
        if ((++photo_cnt % 5) == 0) hw2d_stats_dump();
    }
}

/* ================= 拍照 (LVGL/core0: 仅抓帧 + 投递) ================= */
static void take_photo(void)
{
    const app_camera_frame_t *f;

    if (!s_canvas_buf || !s_photo_q) {
        toast_show("无画面");
        return;
    }
    int64_t t0 = esp_timer_get_time();

    /* 闪光: hw2d_fill 全屏白, 立即刷新 */
    hw2d_fill(s_canvas_buf, UI_W * UI_H, 0xFFFF);
    lv_obj_invalidate(s_canvas);
    lv_refr_now(NULL);

    int slot = snap_acquire();
    if (slot < 0) {
        PVC_EV("photo_drop reason=pool");
        toast_show("处理中, 稍候再拍");
        return;
    }
    uint16_t *snap = s_snap_pool[slot];

    /* 抓最新帧: esp_camera 阻塞至 DMA 完成一帧 (闪白后的画面), 零拷贝读取 */
    f = app_camera_grab();
    if (!f) {
        snap_release(slot);
        toast_show("抓帧失败");
        return;
    }
    static uint32_t s_seq = 0;
    photo_job_t job = {
        .snap = snap,
        .slot = slot,
        .w = f->width, .h = f->height,      /* VGA 正常 / QVGA 降级均可 */
        .filter = s_active_filter,          /* 按值快照, 与 UI 后续修改解耦 */
        .smooth = s_smooth,
        .white = s_white,
        .sticker = s_sticker,
        .mirror = s_mirror,
        .fid = s_filter,
        .seq = s_seq,
        .t_shutter = t0,
    };
    job.fbox_valid = (s_sticker >= 0) && pvc_face_latest(&job.fbox);
    hw2d_copy(snap, f->buf, job.w * job.h * 2);
    app_camera_release();
    job.grab_ms = (int)((esp_timer_get_time() - t0) / 1000);

    if (xQueueSend(s_photo_q, &job, 0) != pdTRUE) {
        snap_release(slot);
        PVC_EV("photo_drop reason=busy");
        toast_show("处理中, 稍候再拍");
        return;
    }
    s_seq++;
    /* 磨皮/滤镜/存卡/编码/上传由 core1 worker 异步完成, 预览立即恢复 */

    /* 拍后 6s 预设配文 (worker 编码后等 s_caption_sem 信箱):
     * 不弹面板 worker 也只是白等 6s 后无配文上传 (真机曾遗漏此调用) */
    open_caption_panel();
}

/* ================= 预设配文面板 (拍后 6s 内可选) ================= */
static lv_obj_t *s_caption_panel_obj;

static void caption_timeout_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    if (s_panel && s_panel == s_caption_panel_obj) close_panel();
    s_caption_panel_obj = NULL;
}

static void on_caption_click(lv_event_t *e)
{
    static const char *const k_phrases[] = {
        "想你了", "今天也在场", "分你一朵云", "晚点见"
    };
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < 4) {
        strncpy(s_caption_sel, k_phrases[idx], sizeof(s_caption_sel) - 1);
        PVC_EV("caption sel=%d", idx);
        if (s_caption_sem) xSemaphoreGive(s_caption_sem);
    }
    s_caption_panel_obj = NULL;
    close_panel();
}

static void open_caption_panel(void)
{
    static const char *const k_phrases[] = {
        "想你了", "今天也在场", "分你一朵云", "晚点见"
    };
    if (s_caption_sem) xSemaphoreTake(s_caption_sem, 0);   /* 清残留 */
    s_caption_sel[0] = '\0';

    lv_obj_t *p = panel_create(56);
    s_caption_panel_obj = p;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(p);
        lv_obj_set_size(b, 74, 40);
        lv_obj_set_pos(b, 4 + i * 78, 8);
        lv_obj_set_style_bg_color(b, lv_color_make(0x2f, 0x36, 0x45), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, on_caption_click, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, k_phrases[i]);
        /* 5 字 x16px = 80px 超按钮宽 74: 限宽换行居中, 不截字 */
        lv_obj_set_width(l, 70);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_center(l);
    }
    lv_timer_create(caption_timeout_cb, 6000, NULL);
}

/* ================= 面板控制 ================= */
static void close_panel(void)
{
    if (s_panel) {
        lv_obj_del(s_panel);
        s_panel = NULL;
    }
}

/* 通用: 底部横条容器 */
static lv_obj_t *panel_create(uint32_t h)
{
    close_panel();
    s_panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_panel, UI_W, h);
    lv_obj_set_pos(s_panel, 0, UI_H - BAR_H - h);
    lv_obj_set_style_bg_color(s_panel, lv_color_make(0x17, 0x14, 0x1c), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_shadow_width(s_panel, 0, 0);
    return s_panel;
}

/* 面板里的缩略+名字按钮 */
static void panel_item(lv_obj_t *panel, const char *txt, int thumb_idx,
                       int x, lv_event_cb_t cb, int idx)
{
    lv_obj_t *box = lv_btn_create(panel);
    lv_obj_set_size(box, 48, 46);
    lv_obj_set_pos(box, x, 6);
    lv_obj_set_style_bg_color(box, lv_color_make(0x2b, 0x25, 0x30), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_shadow_width(box, 0, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_add_event_cb(box, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    if (thumb_idx >= 0 && s_thumb_bufs[thumb_idx]) {
        /* 复用共享缩略图缓冲 (只读, 滤镜条与面板同步刷新) */
        lv_obj_t *cv = lv_canvas_create(box);
        lv_canvas_set_buffer(cv, (void *)s_thumb_bufs[thumb_idx],
                             THUMB_SZ, THUMB_SZ, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(cv, 2, 2);
        lv_obj_set_style_bg_opa(cv, LV_OPA_TRANSP, 0);
    }
    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
}

/* ---------- 滤镜面板 ---------- */
static void on_filter_click(lv_event_t *e)
{
    s_filter = (int)(intptr_t)lv_event_get_user_data(e);
    update_active_filter();
    s_thumb_dirty = true;
    close_panel();
    update_status();
}

static const char *filter_name(int f);

static void open_filter_panel(void)
{
    /* 10 项 (6 基础 + 4 CCD 机型) 超出屏宽, 横向滑动选择 */
    lv_obj_t *p = panel_create(56);
    lv_obj_set_scroll_dir(p, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    for (int i = 0; i < FILTER_EXT_COUNT; i++) {
        panel_item(p, filter_name(i), i < HW2D_FILTER_MAX ? i : -1,
                   6 + i * 52, on_filter_click, i);
    }
}

/* ---------- 贴纸面板 ---------- */
static void on_sticker_click(lv_event_t *e)
{
    s_sticker = (int)(intptr_t)lv_event_get_user_data(e);
    close_panel();
    if (s_sticker >= 0 && !pvc_face_init()) {
        toast_show("人脸跟随不可用");     /* 模型缺失: 贴纸回落固定位置 */
    }
}

static void open_sticker_panel(void)
{
    lv_obj_t *p = panel_create(56);
    static const char *const stickers[] = { "兔耳", "墨镜", "帽子", "皇冠", "猫须", "爱心" };
    for (int i = 0; i < 6; i++) {
        panel_item(p, stickers[i], -1, 6 + i * 52, on_sticker_click, i);
    }
}

/* ---------- 美颜面板 (美白 + 磨皮双滑块) ---------- */
static lv_obj_t *s_sl_white, *s_sl_smooth;

static void on_slider_change(lv_event_t *e)
{
    (void)e;
    if (s_sl_white) {
        s_white = (int)lv_slider_get_value(s_sl_white);
        update_active_filter();
        update_status();
    }
    if (s_sl_smooth) {
        s_smooth = (int)lv_slider_get_value(s_sl_smooth);
        update_status();
    }
}

static void open_beauty_panel(void)
{
    lv_obj_t *p = panel_create(84);

    lv_obj_t *l1 = lv_label_create(p);
    lv_label_set_text(l1, "美白");
    lv_obj_set_pos(l1, 10, 8);
    lv_obj_set_style_text_color(l1, lv_color_white(), 0);

    s_sl_white = lv_slider_create(p);
    lv_obj_set_size(s_sl_white, 120, 12);
    lv_obj_set_pos(s_sl_white, 54, 10);
    lv_slider_set_range(s_sl_white, 0, 100);
    lv_slider_set_value(s_sl_white, s_white, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_sl_white, on_slider_change, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *l2 = lv_label_create(p);
    lv_label_set_text(l2, "磨皮");
    lv_obj_set_pos(l2, 10, 46);
    lv_obj_set_style_text_color(l2, lv_color_white(), 0);

    s_sl_smooth = lv_slider_create(p);
    lv_obj_set_size(s_sl_smooth, 120, 12);
    lv_obj_set_pos(s_sl_smooth, 54, 48);
    lv_slider_set_range(s_sl_smooth, 0, 100);
    lv_slider_set_value(s_sl_smooth, s_smooth, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_sl_smooth, on_slider_change, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *done = lv_btn_create(p);
    lv_obj_set_size(done, 88, 30);
    lv_obj_set_pos(done, 210, 26);
    lv_obj_add_event_cb(done, panel_done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(done);
    lv_label_set_text(dl, "完成");
    lv_obj_center(dl);
}

/* 面板"完成" / 相册返回 等命名回调 */
static void panel_done_cb(lv_event_t *e) { (void)e; close_panel(); }

/* ================= 相册 ================= */
static lv_obj_t *s_album_panel = NULL;
static lv_obj_t *s_album_grid[6];
static uint16_t *s_album_bufs[6];
static lv_obj_t *s_view_canvas = NULL;
static uint16_t *s_view_buf = NULL;
static char s_album_paths[MAX_ALBUM_ITEMS][64];
static int s_album_ram[MAX_ALBUM_ITEMS];   /* >=0: PSRAM 环第 i 新; -1: 文件 */
static int s_album_n = 0;

static void album_show_full(int item)
{
    uint32_t w, h;
    uint16_t *tmp;

    if (!s_view_buf || !s_view_canvas) return;
    tmp = PSRAM_MALLOC(FRAME_BYTES);  /* VGA JPEG 以 1/2 档解码, 恰为 320x240 */
    if (!tmp) {
        toast_show("内存不足");
        return;
    }
    int rc = (s_album_ram[item] >= 0)
        ? ram_album_decode(s_album_ram[item], tmp, UI_W, UI_H, &w, &h)
        : load_jpg_565(s_album_paths[item], tmp, UI_W, UI_H, &w, &h);
    if (rc != 0) {
        PSRAM_FREE(tmp);
        toast_show("读取失败");
        return;
    }
    /* hw2d_scale 要求源与目标分离: 源=tmp, 目标=s_view_buf */
    hw2d_scale_stat(tmp, w, h, s_view_buf, UI_W, UI_H);
    PSRAM_FREE(tmp);

    /* 显示全屏查看, 隐藏网格 */
    for (int i = 0; i < MAX_ALBUM_ITEMS; i++) {
        if (s_album_grid[i]) lv_obj_add_flag(s_album_grid[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_view_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_view_canvas);
    lv_obj_invalidate(s_view_canvas);
}

static void on_album_item(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    album_show_full(idx);
}

static void open_album(void)
{
    DIR *d;
    struct dirent *ent;
    int n = 0;

    close_panel();
    if (!s_album_panel) return;
    lv_obj_clear_flag(s_album_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_album_panel);

    for (int i = 0; i < MAX_ALBUM_ITEMS; i++) s_album_ram[i] = -1;
    d = opendir("/sdcard/DCIM");
    if (d) {
        while (n < MAX_ALBUM_ITEMS && (ent = readdir(d)) != NULL) {
            if (strstr(ent->d_name, ".jpg") || strstr(ent->d_name, ".JPG")) {
                /* 限长复制文件名, 防止 snprintf 截断 (-Werror=format-truncation) */
                char name[48];
                strncpy(name, ent->d_name, sizeof(name) - 1);
                name[sizeof(name) - 1] = '\0';
                snprintf(s_album_paths[n], sizeof(s_album_paths[0]),
                         "/sdcard/DCIM/%s", name);
                n++;
            }
        }
        closedir(d);
    } else {
        /* 无 SD 卡: PSRAM 环兜底 (最新在前) */
        for (int i = 0; i < s_ram_cnt && n < MAX_ALBUM_ITEMS; i++) {
            s_album_ram[n++] = i;
        }
    }
    s_album_n = n;

    for (int i = 0; i < MAX_ALBUM_ITEMS; i++) {
        lv_obj_t *cell = s_album_grid[i];
        if (!cell) continue;
        if (i < n && s_album_bufs[i]) {
            uint32_t w, h;
            /* 1/4 档解码 (VGA -> 160x120) 再缩到 96x72, 免全尺寸解码 */
            uint16_t *tmp = PSRAM_MALLOC(160 * 120 * 2);
            if (tmp) {
                int rc = (s_album_ram[i] >= 0)
                    ? ram_album_decode(s_album_ram[i], tmp, 160, 120, &w, &h)
                    : load_jpg_565(s_album_paths[i], tmp, 160, 120, &w, &h);
                if (rc == 0) {
                    /* 源=tmp, 目标=缩略缓冲 (分离) */
                    hw2d_scale_stat(tmp, w, h, s_album_bufs[i], 96, 72);
                }
                PSRAM_FREE(tmp);
            }
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(cell);
        } else {
            lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void close_album(void)
{
    if (s_album_panel) lv_obj_add_flag(s_album_panel, LV_OBJ_FLAG_HIDDEN);
}

/* 相册 / 全屏查看返回 */
static void on_album_back_cb(lv_event_t *e)
{
    (void)e;
    close_album();
}

static void on_view_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_view_canvas) lv_obj_add_flag(s_view_canvas, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < MAX_ALBUM_ITEMS; i++) {
        if (s_album_grid[i]) lv_obj_clear_flag(s_album_grid[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ================= 点赞反馈 (飘字动画 + 音效) ================= */
static void like_anim_y(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void spawn_like_float(int count)
{
    if (count > 3) count = 3;
    for (int i = 0; i < count; i++) {
        lv_obj_t *l = lv_label_create(lv_scr_act());
        lv_label_set_text(l, "+1 赞");
        lv_obj_set_style_text_color(l, lv_color_make(0xE8, 0x4A, 0x6A), 0);
        lv_obj_set_pos(l, 136 + i * 26, 190);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, l);
        lv_anim_set_exec_cb(&a, like_anim_y);
        lv_anim_set_values(&a, 190, 70);
        lv_anim_set_duration(&a, 1200);
        lv_anim_set_delay(&a, (uint32_t)i * 150);
        lv_anim_start(&a);
        lv_obj_delete_delayed(l, 1700 + (uint32_t)i * 150);
    }
}

/* ================= 好友 feed 浏览 (规范 §3) ================= */
static lv_obj_t *s_feed_panel = NULL;
static lv_obj_t *s_feed_canvas = NULL;
static uint16_t *s_feed_buf = NULL;           /* 320x240 RGB565 (PSRAM) */
static lv_obj_t *s_feed_author, *s_feed_caption, *s_feed_counter;
static pvc_feed_item_t s_feed_items[PVC_FEED_MAX];
static int s_feed_n = 0, s_feed_idx = 0;
static bool s_feed_auto = false;          /* 到达仪式自动展示中 (交互即取消) */
static lv_timer_t *s_arrival_timer = NULL;


/* 解码并显示 s_feed_items[s_feed_idx] */
static void render_feed(void)
{
    if (!s_feed_buf || s_feed_n == 0) return;
    if (s_feed_idx < 0) s_feed_idx = 0;
    if (s_feed_idx >= s_feed_n) s_feed_idx = s_feed_n - 1;
    const pvc_feed_item_t *it = &s_feed_items[s_feed_idx];
    int64_t t0 = esp_timer_get_time();

    uint8_t *jpg = PSRAM_MALLOC(160 * 1024);
    int len = -1;
    if (jpg) len = pvc_feed_read_jpeg(it->photo_id, jpg, 160 * 1024);

    bool ok = false;
    uint32_t w = 0, h = 0;
    if (len > 0 && pvc_jpeg_dims(jpg, (size_t)len, &w, &h) &&
        w == UI_W && h == UI_H) {
        /* jpg2rgb565 输出小端 RGB565 (QEMU 哨兵实证), 直接解码进 canvas */
        ok = jpg2rgb565(jpg, (size_t)len, (uint8_t *)s_feed_buf, JPG_SCALE_NONE);
    }
    if (jpg) PSRAM_FREE(jpg);

    if (!ok) {
        hw2d_fill(s_feed_buf, UI_W * UI_H, rgb565(0x20, 0x26, 0x34));
        ESP_LOGW(TAG, "feed decode failed: %s (%lux%lu len=%d)",
                 it->photo_id, (unsigned long)w, (unsigned long)h, len);
    } else {
        PVC_EV("perf_feed_decode ms=%d bytes=%d",
               (int)((esp_timer_get_time() - t0) / 1000), len);
    }

    char line[112];
    snprintf(line, sizeof(line), "%s", it->author[0] ? it->author : "?");
    lv_label_set_text(s_feed_author, line);
    lv_label_set_text(s_feed_caption, it->caption);
    snprintf(line, sizeof(line), "%d/%d", s_feed_idx + 1, s_feed_n);
    lv_label_set_text(s_feed_counter, line);
    lv_obj_invalidate(s_feed_canvas);
}

static void open_feed(void)
{
    s_feed_auto = false;
    close_panel();
    s_feed_n = pvc_feed_snapshot(s_feed_items, PVC_FEED_MAX);
    pvc_net_signal_feed();                 /* 顺手触发一次刷新 */
    if (s_feed_n == 0) {
        toast_show("暂无好友照片");
        return;
    }
    if (s_feed_idx >= s_feed_n) s_feed_idx = 0;
    lv_obj_clear_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_feed_panel);
    render_feed();
}

static void on_feed_back(lv_event_t *e)
{
    (void)e;
    s_feed_auto = false;
    lv_obj_add_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN);
}

static void on_feed_prev(lv_event_t *e)
{
    (void)e;
    s_feed_auto = false;
    if (s_feed_n == 0) return;
    s_feed_idx = (s_feed_idx + s_feed_n - 1) % s_feed_n;
    render_feed();
}

static void on_feed_next(lv_event_t *e)
{
    (void)e;
    s_feed_auto = false;
    if (s_feed_n == 0) return;
    s_feed_idx = (s_feed_idx + 1) % s_feed_n;
    render_feed();
}

static void do_like(void)
{
    if (s_feed_n == 0) return;
    pvc_feed_react_async(s_feed_items[s_feed_idx].photo_id, "heart");
    pvc_net_signal_feed();
    pvc_sound_play(PVC_SND_LIKE);
    spawn_like_float(1);
}

static void on_feed_heart(lv_event_t *e)
{
    (void)e;
    s_feed_auto = false;
    do_like();
}

/* 双击照片点赞 (400ms 内两次点击) */
static void on_feed_canvas_click(lv_event_t *e)
{
    (void)e;
    s_feed_auto = false;
    static uint32_t s_last_ms;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s_last_ms < 400) {
        s_last_ms = 0;
        PVC_EV("like via=double_tap");
        do_like();
    } else {
        s_last_ms = now;
    }
}

/* pvc_config 回调 (联网任务): 应用 web 下发的配置 */
void ui_apply_remote_config(const pvc_config_t *cfg)
{
    if (!bsp_display_lock(1000)) return;

    /* filter_id (web 清单) -> 端侧滤镜, 与上传映射 k_filter_api_id 互逆 */
    if (strcmp(cfg->filter_id, "none") == 0)       s_filter = HW2D_FILTER_ORIGINAL;
    else if (strcmp(cfg->filter_id, "warm") == 0)  s_filter = HW2D_FILTER_WARM;
    else if (strcmp(cfg->filter_id, "bw") == 0)    s_filter = HW2D_FILTER_BW;
    else if (strcmp(cfg->filter_id, "film") == 0)  s_filter = HW2D_FILTER_VINTAGE;
    else if (strcmp(cfg->filter_id, "vivid") == 0) s_filter = HW2D_FILTER_COOL;
    /* 未知 id: 保持当前滤镜 */

    s_white = (cfg->beauty < 0) ? 0 : (cfg->beauty > 100 ? 100 : cfg->beauty);

    /* server sticker 枚举 none/star/date 与端侧现有贴纸素材不对应:
     * none 关闭贴纸; star/date 待素材到位后映射 */
    if (strcmp(cfg->sticker, "none") == 0) s_sticker = -1;
    /* play_type (beauty/ccd/template): 端侧当前单一拍摄模式, 记录忽略 */

    update_active_filter();
    s_thumb_dirty = true;
    update_status();
    toast_show("已应用新配置");
    bsp_display_unlock();
}

/* 到达仪式自动展示的收起定时器 (3s; 期间用户交互则保留页面) */
static void arrival_hide_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    s_arrival_timer = NULL;
    if (s_feed_auto && s_feed_panel) {
        lv_obj_add_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN);
    }
    s_feed_auto = false;
}

/* pvc_net 回调 (联网任务): feed 有更新 / 本人照片被赞 */
void ui_net_feed_updated(int total, int fresh, int new_likes)
{
    if (!bsp_display_lock(1000)) return;

    if (new_likes > 0) {                  /* 被赞: 飘字 + 音效 */
        pvc_sound_play(PVC_SND_LIKE);
        spawn_like_float(new_likes);
    }

    if (fresh > 0) {
        /* 到达仪式: 亮屏 + 提示音 + 自动展示最新一张 3s (docs T17) */
        pvc_sound_play(PVC_SND_DING);
        bsp_display_backlight_on();
        lv_display_trigger_activity(NULL);   /* 重置省电空闲计时 */
        s_feed_n = pvc_feed_snapshot(s_feed_items, PVC_FEED_MAX);
        PVC_EV("arrival fresh=%d shown=%d", fresh, (int)(s_feed_n > 0));
        if (s_feed_n > 0 && s_feed_panel && s_feed_buf) {
            s_feed_idx = 0;
            s_feed_auto = true;
            lv_obj_clear_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_feed_panel);
            render_feed();
            if (s_arrival_timer) lv_timer_del(s_arrival_timer);
            s_arrival_timer = lv_timer_create(arrival_hide_cb, 3000, NULL);
        }
        char msg[32];
        snprintf(msg, sizeof(msg), "新照片 +%d", fresh);
        toast_show(msg);
    } else if (s_feed_panel && !lv_obj_has_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN)) {
        /* 浏览页开着时就地刷新 */
        s_feed_n = pvc_feed_snapshot(s_feed_items, PVC_FEED_MAX);
        if (s_feed_n == 0) {
            lv_obj_add_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            render_feed();
        }
    }
    (void)total;
    bsp_display_unlock();
}

/* ================= 状态栏时钟 ================= */
static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_status_l) return;
    char hhmm[8];
    pvc_clock_hhmm(hhmm, sizeof(hhmm));
    lv_label_set_text(s_status_l, hhmm);
}

/* ================= 状态栏 / 卡片 ================= */
static const char *filter_name(int f)
{
    static const char *const n[HW2D_FILTER_MAX] = {
        "原图", "净白", "暖阳", "冷调", "黑白", "复古"
    };
    if (f >= FILTER_CCD_BASE) return k_ccd_cams[f - FILTER_CCD_BASE].name;
    return n[f];
}

static void update_status(void)
{
    if (s_status_c) {
        lv_label_set_text(s_status_c, filter_name(s_filter));
    }
}

/* ================= 主控制条按钮 ================= */
/* 工具栏常规按钮统一样式 (深色半透明圆角) */
static void style_tool_btn(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, lv_color_make(0x18, 0x16, 0x1d), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_make(0xff, 0xf3, 0xe9), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
}
static void btn_shutter_cb(lv_event_t *e) { (void)e; take_photo(); }
static void btn_sticker_cb(lv_event_t *e) { (void)e; open_sticker_panel(); }
static void btn_album_cb(lv_event_t *e)   { (void)e; open_album(); }
static void btn_filter_cb(lv_event_t *e)  { (void)e; open_filter_panel(); }
static void btn_beauty_cb(lv_event_t *e)  { (void)e; open_beauty_panel(); }
static void btn_mirror_cb(lv_event_t *e)
{
    (void)e;
    s_mirror = !s_mirror;
    if (s_mirror_label) lv_label_set_text(s_mirror_label, s_mirror ? "镜像" : "非镜像");
    s_thumb_dirty = true;
    toast_show(s_mirror ? "自拍镜像" : "非镜像");
}
static void btn_feed_cb(lv_event_t *e)
{
    (void)e;
    open_feed();
}

/* 工程模式 (docs/02 §6): 长按"动态"键 -> 串口打印 token 前 8 位等调试信息 */
static void btn_flip_long_cb(lv_event_t *e)
{
    (void)e;
    pvc_net_debug_dump();
    toast_show("ENG dump -> serial");
}

/* ================= 创建 UI ================= */
void ui_beauty_camera_create(void)
{
    /* 真机实测: 本函数在 main 任务跑, LVGL 刷新任务已在并发运行 ——
     * 建树全程必须持显示锁, 否则样式链表被并发遍历 LoadProhibited */
    if (!bsp_display_lock(3000)) {
        ESP_LOGE(TAG, "ui create: display lock timeout");
        return;
    }
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_make(0x12, 0x0f, 0x16), 0);

    hw2d_init();
    update_active_filter();

    /* ---------- 全屏预览 canvas ---------- */
    s_canvas_buf = PSRAM_MALLOC(FRAME_BYTES);
    if (!s_canvas_buf) { ESP_LOGE(TAG, "canvas PSRAM alloc failed"); return; }

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, UI_W, UI_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_style_bg_opa(s_canvas, LV_OPA_TRANSP, 0);

    /* ---------- 状态栏 ---------- */
    lv_obj_t *status = lv_obj_create(scr);
    lv_obj_set_size(status, UI_W, STATUS_H);
    lv_obj_set_pos(status, 0, 0);
    lv_obj_set_style_bg_color(status, lv_color_make(0x12, 0x0f, 0x16), 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_50, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_pad_all(status, 0, 0);
    lv_obj_set_style_radius(status, 0, 0);
    lv_obj_set_style_shadow_width(status, 0, 0);

    s_status_l = lv_label_create(status);
    lv_label_set_text(s_status_l, "--:--");
    lv_obj_set_pos(s_status_l, 10, 4);
    lv_obj_set_style_text_color(s_status_l, lv_color_make(0xff, 0xf3, 0xe9), 0);

    s_status_c = lv_label_create(status);
    lv_obj_align(s_status_c, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_text_color(s_status_c, lv_color_make(0xff, 0xf3, 0xe9), 0);

    s_status_r = lv_label_create(status);
    lv_label_set_text(s_status_r, "100%");
    lv_obj_set_pos(s_status_r, UI_W - 42, 4);
    lv_obj_set_style_text_color(s_status_r, lv_color_make(0xf4, 0x8c, 0x7f), 0);
    /* 离线状态可点击进入重新配网 (换 WiFi 环境); 扩大触摸热区 */
    lv_obj_add_flag(s_status_r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_status_r, 14);
    lv_obj_add_event_cb(s_status_r, on_net_status_click, LV_EVENT_CLICKED, NULL);

    /* 滤镜缩略图只在弹出面板显示; 不常驻遮挡低分辨率取景画面。 */
    s_thumb_raw = PSRAM_MALLOC(THUMB_SZ * THUMB_SZ * 2);
    for (int i = 0; i < HW2D_FILTER_MAX; i++) {
        s_thumb_bufs[i] = PSRAM_MALLOC(THUMB_SZ * THUMB_SZ * 2);
    }

    /* ---------- 底部工具栏 ---------- */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, UI_W, BAR_H);
    lv_obj_set_pos(bar, 0, UI_H - BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_make(0x12, 0x10, 0x16), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_shadow_width(bar, 0, 0);

    const int by = 10;
    /* 滤镜: 长按进入贴纸 */
    lv_obj_t *b_filter = lv_btn_create(bar);
    lv_obj_set_size(b_filter, 52, 32);
    lv_obj_set_pos(b_filter, 8, by);
    style_tool_btn(b_filter);
    lv_obj_add_event_cb(b_filter, btn_filter_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b_filter, btn_sticker_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *b_filter_l = lv_label_create(b_filter);
    lv_label_set_text(b_filter_l, "滤镜");
    lv_obj_set_style_text_color(b_filter_l, lv_color_make(0xff, 0xf3, 0xe9), 0);
    lv_obj_center(b_filter_l);

    /* 美颜 */
    lv_obj_t *b_beauty = lv_btn_create(bar);
    lv_obj_set_size(b_beauty, 52, 32);
    lv_obj_set_pos(b_beauty, 70, by);
    style_tool_btn(b_beauty);
    lv_obj_add_event_cb(b_beauty, btn_beauty_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b_beauty_l = lv_label_create(b_beauty);
    lv_label_set_text(b_beauty_l, "美颜");
    lv_obj_set_style_text_color(b_beauty_l, lv_color_make(0xff, 0xf3, 0xe9), 0);
    lv_obj_center(b_beauty_l);

    /* 快门: 奶油外圈 + 珊瑚内芯; 长按进入相册 */
    lv_obj_t *b_shutter = lv_btn_create(bar);
    lv_obj_set_size(b_shutter, 52, 52);
    lv_obj_set_pos(b_shutter, 134, 0);
    lv_obj_set_style_bg_color(b_shutter, lv_color_make(0xff, 0xf3, 0xe9), 0);
    lv_obj_set_style_radius(b_shutter, 26, 0);
    lv_obj_set_style_border_width(b_shutter, 0, 0);
    lv_obj_set_style_shadow_width(b_shutter, 0, 0);
    lv_obj_set_style_pad_all(b_shutter, 0, 0);
    lv_obj_add_event_cb(b_shutter, btn_shutter_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b_shutter, btn_album_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *shutter_inner = lv_obj_create(b_shutter);
    lv_obj_set_size(shutter_inner, 38, 38);
    lv_obj_center(shutter_inner);
    lv_obj_set_style_bg_color(shutter_inner, lv_color_make(0xf4, 0x8c, 0x7f), 0);
    lv_obj_set_style_bg_opa(shutter_inner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(shutter_inner, 19, 0);
    lv_obj_set_style_border_width(shutter_inner, 0, 0);
    lv_obj_set_style_pad_all(shutter_inner, 0, 0);
    lv_obj_remove_flag(shutter_inner, LV_OBJ_FLAG_CLICKABLE);

    /* 自拍方向 */
    lv_obj_t *b_mirror = lv_btn_create(bar);
    lv_obj_set_size(b_mirror, 52, 32);
    lv_obj_set_pos(b_mirror, 198, by);
    style_tool_btn(b_mirror);
    lv_obj_add_event_cb(b_mirror, btn_mirror_cb, LV_EVENT_CLICKED, NULL);
    s_mirror_label = lv_label_create(b_mirror);
    lv_label_set_text(s_mirror_label, "镜像");
    lv_obj_set_style_text_color(s_mirror_label, lv_color_make(0xff, 0xf3, 0xe9), 0);
    lv_obj_center(s_mirror_label);

    /* 动态 (feed, §3); 长按 = 工程模式 */
    lv_obj_t *b_feed = lv_btn_create(bar);
    lv_obj_set_size(b_feed, 52, 32);
    lv_obj_set_pos(b_feed, 260, by);
    style_tool_btn(b_feed);
    lv_obj_add_event_cb(b_feed, btn_feed_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b_feed, btn_flip_long_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *b_feed_l = lv_label_create(b_feed);
    lv_label_set_text(b_feed_l, "动态");
    lv_obj_set_style_text_color(b_feed_l, lv_color_make(0xff, 0xf3, 0xe9), 0);
    lv_obj_center(b_feed_l);

    /* ---------- 相册界面 (常驻, 默认隐藏) ---------- */
    s_album_panel = lv_obj_create(scr);
    lv_obj_set_size(s_album_panel, UI_W, UI_H);
    lv_obj_set_pos(s_album_panel, 0, 0);
    lv_obj_set_style_bg_color(s_album_panel, lv_color_make(0x10, 0x14, 0x1e), 0);
    lv_obj_set_style_border_width(s_album_panel, 0, 0);
    lv_obj_set_style_pad_all(s_album_panel, 0, 0);
    lv_obj_set_style_radius(s_album_panel, 0, 0);
    lv_obj_set_style_shadow_width(s_album_panel, 0, 0);
    lv_obj_add_flag(s_album_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_btn_create(s_album_panel);
    lv_obj_set_size(back, 52, 28);
    lv_obj_set_pos(back, 8, 6);
    lv_obj_add_event_cb(back, on_album_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_l = lv_label_create(back);
    lv_label_set_text(back_l, "< 返回");
    lv_obj_center(back_l);

    /* 3x2 网格 */
    s_view_buf = PSRAM_MALLOC(FRAME_BYTES);
    for (int i = 0; i < MAX_ALBUM_ITEMS; i++) {
        s_album_bufs[i] = PSRAM_MALLOC(96 * 96 * 2);
        lv_obj_t *cell = lv_btn_create(s_album_panel);
        lv_obj_set_size(cell, 100, 76);
        int col = i % 3, row = i / 3;
        lv_obj_set_pos(cell, 4 + col * 106, 42 + row * 84);
        lv_obj_set_style_bg_color(cell, lv_color_make(0x20, 0x26, 0x34), 0);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_set_style_shadow_width(cell, 0, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_add_event_cb(cell, on_album_item, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
        s_album_grid[i] = cell;
        if (s_album_bufs[i]) {
            lv_obj_t *cv = lv_canvas_create(cell);
            lv_canvas_set_buffer(cv, s_album_bufs[i], 96, 72, LV_COLOR_FORMAT_RGB565);
            lv_obj_set_pos(cv, 2, 2);
            lv_obj_set_style_bg_opa(cv, LV_OPA_TRANSP, 0);
        }
    }

    /* 全屏查看画布 (叠在相册之上) */
    s_view_canvas = NULL;
    if (s_view_buf) {
        s_view_canvas = lv_canvas_create(s_album_panel);
        lv_canvas_set_buffer(s_view_canvas, s_view_buf, UI_W, UI_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_view_canvas, 0, 0);
        lv_obj_set_style_bg_opa(s_view_canvas, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s_view_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *v_back = lv_btn_create(s_album_panel);
    lv_obj_set_size(v_back, 52, 28);
    lv_obj_set_pos(v_back, 8, 6);
    lv_obj_add_event_cb(v_back, on_view_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *vb_l = lv_label_create(v_back);
    lv_label_set_text(vb_l, "< 返回");
    lv_obj_center(vb_l);

    /* ---------- 好友 feed 浏览页 (常驻, 默认隐藏, §3) ---------- */
    s_feed_buf = PSRAM_MALLOC(FRAME_BYTES);
    s_feed_panel = lv_obj_create(scr);
    lv_obj_set_size(s_feed_panel, UI_W, UI_H);
    lv_obj_set_pos(s_feed_panel, 0, 0);
    lv_obj_set_style_bg_color(s_feed_panel, lv_color_make(0x10, 0x14, 0x1e), 0);
    lv_obj_set_style_border_width(s_feed_panel, 0, 0);
    lv_obj_set_style_pad_all(s_feed_panel, 0, 0);
    lv_obj_set_style_radius(s_feed_panel, 0, 0);
    lv_obj_set_style_shadow_width(s_feed_panel, 0, 0);
    lv_obj_add_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN);

    if (s_feed_buf) {
        s_feed_canvas = lv_canvas_create(s_feed_panel);
        lv_canvas_set_buffer(s_feed_canvas, s_feed_buf, UI_W, UI_H,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_feed_canvas, 0, 0);
        lv_obj_set_style_bg_opa(s_feed_canvas, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s_feed_canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_feed_canvas, on_feed_canvas_click,
                            LV_EVENT_CLICKED, NULL);
    }

    /* 底部信息条: 作者 + 配文 + 计数 */
    lv_obj_t *fbar = lv_obj_create(s_feed_panel);
    lv_obj_set_size(fbar, UI_W, 36);
    lv_obj_set_pos(fbar, 0, UI_H - 36);
    lv_obj_set_style_bg_color(fbar, lv_color_make(0x00, 0x00, 0x00), 0);
    lv_obj_set_style_bg_opa(fbar, LV_OPA_60, 0);
    lv_obj_set_style_border_width(fbar, 0, 0);
    lv_obj_set_style_pad_all(fbar, 0, 0);
    lv_obj_set_style_radius(fbar, 0, 0);
    lv_obj_set_style_shadow_width(fbar, 0, 0);

    s_feed_author = lv_label_create(fbar);
    lv_obj_set_pos(s_feed_author, 8, 2);
    lv_obj_set_style_text_color(s_feed_author, lv_color_white(), 0);
    s_feed_caption = lv_label_create(fbar);
    lv_obj_set_pos(s_feed_caption, 8, 19);
    lv_obj_set_width(s_feed_caption, UI_W - 120);
    lv_label_set_long_mode(s_feed_caption, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_feed_caption, lv_color_make(0xc0, 0xc6, 0xd2), 0);
    s_feed_counter = lv_label_create(fbar);
    lv_obj_set_pos(s_feed_counter, UI_W - 44, 2);
    lv_obj_set_style_text_color(s_feed_counter, lv_color_white(), 0);

    /* 返回 / 上一张 / 下一张 / 点赞 */
    lv_obj_t *f_back = lv_btn_create(s_feed_panel);
    lv_obj_set_size(f_back, 52, 28);
    lv_obj_set_pos(f_back, 8, 6);
    lv_obj_add_event_cb(f_back, on_feed_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *f_back_l = lv_label_create(f_back);
    lv_label_set_text(f_back_l, "< 返回");
    lv_obj_center(f_back_l);

    lv_obj_t *f_prev = lv_btn_create(s_feed_panel);
    lv_obj_set_size(f_prev, 36, 48);
    lv_obj_set_pos(f_prev, 4, (UI_H - 48) / 2);
    lv_obj_set_style_bg_opa(f_prev, LV_OPA_40, 0);
    lv_obj_add_event_cb(f_prev, on_feed_prev, LV_EVENT_CLICKED, NULL);
    lv_obj_t *f_prev_l = lv_label_create(f_prev);
    lv_label_set_text(f_prev_l, "<");   /* LV_SYMBOL 在 CJK 字体下缺字 */
    lv_obj_center(f_prev_l);

    lv_obj_t *f_next = lv_btn_create(s_feed_panel);
    lv_obj_set_size(f_next, 36, 48);
    lv_obj_set_pos(f_next, UI_W - 40, (UI_H - 48) / 2);
    lv_obj_set_style_bg_opa(f_next, LV_OPA_40, 0);
    lv_obj_add_event_cb(f_next, on_feed_next, LV_EVENT_CLICKED, NULL);
    lv_obj_t *f_next_l = lv_label_create(f_next);
    lv_label_set_text(f_next_l, ">");
    lv_obj_center(f_next_l);

    lv_obj_t *f_heart = lv_btn_create(s_feed_panel);
    lv_obj_set_size(f_heart, 64, 30);          /* "点赞" 2x16px + 内边距 */
    lv_obj_set_pos(f_heart, UI_W - 72, 6);
    lv_obj_set_style_bg_color(f_heart, lv_color_make(0xe8, 0x4a, 0x4a), 0);
    lv_obj_add_event_cb(f_heart, on_feed_heart, LV_EVENT_CLICKED, NULL);
    lv_obj_t *f_heart_l = lv_label_create(f_heart);
    lv_label_set_text(f_heart_l, "点赞");
    lv_obj_set_style_text_color(f_heart_l, lv_color_white(), 0);
    lv_obj_center(f_heart_l);

    update_status();

    /* 预览刷新定时器 (25 FPS): 每 tick grab 最新帧 -> 渲染 -> release */
    lv_timer_create(preview_timer_cb, 40, NULL);

    /* 状态栏真实时钟 (SNTP/RTC 驱动, 每 5s 刷一次足够) */
    lv_timer_create(clock_timer_cb, 5000, NULL);

    bsp_display_unlock();
}

/*
 * 拍照 worker 延迟启动 (真机内部 SRAM 紧张: 8KB 任务栈等 WiFi/BLE 占位后
 * 再分配, 与相机一起由 net 层 wifi_ready 触发)。启动前快门被
 * !s_photo_q 守卫拒绝 —— 相机没开之前本来也拍不了。幂等。
 */
void ui_start_photo_worker(void)
{
    if (s_photo_q) return;
    pvc_sound_init();
    if (!s_caption_sem) s_caption_sem = xSemaphoreCreateBinary();
    for (int i = 0; i < SNAP_POOL_N; i++) {
        if (!s_snap_pool[i]) s_snap_pool[i] = PSRAM_MALLOC(CAP_BYTES);
    }
    if (!s_wk_enc)   s_wk_enc = PSRAM_MALLOC(WK_ENC_CAP);
    if (!s_face_rgb) s_face_rgb = PSRAM_MALLOC(FRAME_BYTES);
    hw2d_yuv_build_luts(hw2d_filter_get(HW2D_FILTER_ORIGINAL), &s_id_luts);
    /* 先发布队列再建任务: worker 首行就 xQueueReceive(s_photo_q);
     * 反过来则 worker 可能读到 NULL。队列先于 worker 存在是安全的 */
    s_photo_q = xQueueCreate(2, sizeof(photo_job_t));
    if (s_photo_q && xTaskCreatePinnedToCore(photo_worker, "photo_wk", 8192,
                                             NULL, 3, NULL, 1) != pdPASS) {
        vQueueDelete(s_photo_q);
        s_photo_q = NULL;
    }
}

/* ================= 联网层 UI 桥 (pvc_net 回调, 任意任务可调) ================= */
static lv_obj_t *s_pair_panel = NULL;

void ui_net_show_pair(const char *code)
{
    if (!bsp_display_lock(1000)) return;
    if (s_pair_panel) {
        lv_obj_del(s_pair_panel);
        s_pair_panel = NULL;
    }
    s_pair_panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_pair_panel, UI_W, UI_H);
    lv_obj_set_pos(s_pair_panel, 0, 0);
    lv_obj_set_style_bg_color(s_pair_panel, lv_color_make(0x0a, 0x0d, 0x14), 0);
    lv_obj_set_style_bg_opa(s_pair_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pair_panel, 0, 0);
    lv_obj_set_style_radius(s_pair_panel, 0, 0);
    lv_obj_move_foreground(s_pair_panel);

    lv_obj_t *t = lv_label_create(s_pair_panel);
    lv_label_set_text(t, "PAIR CODE");
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -50);
    lv_obj_set_style_text_color(t, lv_color_make(0x8a, 0x93, 0xa5), 0);

    /* 配对码大号展示 (左) + 扫码直达 (右) */
    lv_obj_t *c = lv_label_create(s_pair_panel);
    lv_label_set_text(c, code);
    lv_obj_align(c, LV_ALIGN_CENTER, -80, -10);
    lv_obj_set_style_text_color(c, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(c, 8, 0);
    lv_obj_set_style_text_font(c, &lv_font_montserrat_14, 0);

    char qrbuf[160];
    if (PVC_WEB_BASE[0]) {
        snprintf(qrbuf, sizeof(qrbuf), "%s/pair?code=%s&device_id=%s",
                 PVC_WEB_BASE, code, pvc_store_device_id());
    } else {
        snprintf(qrbuf, sizeof(qrbuf), "PVC-PAIR:%s", code);
    }
    lv_obj_t *qr = lv_qrcode_create(s_pair_panel);
    lv_qrcode_set_size(qr, 96);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, qrbuf, (uint32_t)strlen(qrbuf));
    lv_obj_align(qr, LV_ALIGN_CENTER, 70, -12);

    lv_obj_t *h = lv_label_create(s_pair_panel);
    lv_label_set_text(h, "Enter this code on the web\nto bind the device");
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 55);
    lv_obj_set_style_text_color(h, lv_color_make(0x8a, 0x93, 0xa5), 0);
    bsp_display_unlock();
}

/* BLE 配网引导页: 复用配对覆盖层容器 (hide 同一入口) */
void ui_net_show_prov(const char *ble_name, const char *pop)
{
    if (!bsp_display_lock(1000)) return;
    if (s_pair_panel) {
        lv_obj_del(s_pair_panel);
        s_pair_panel = NULL;
    }
    s_pair_panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_pair_panel, UI_W, UI_H);
    lv_obj_set_pos(s_pair_panel, 0, 0);
    lv_obj_set_style_bg_color(s_pair_panel, lv_color_make(0x0a, 0x0d, 0x14), 0);
    lv_obj_set_style_bg_opa(s_pair_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pair_panel, 0, 0);
    lv_obj_set_style_radius(s_pair_panel, 0, 0);
    lv_obj_move_foreground(s_pair_panel);

    lv_obj_t *t = lv_label_create(s_pair_panel);
    lv_label_set_text(t, "WIFI SETUP (BLE)");
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_style_text_color(t, lv_color_make(0x8a, 0x93, 0xa5), 0);

    char line[48];
    lv_obj_t *n = lv_label_create(s_pair_panel);
    snprintf(line, sizeof(line), "Device: %s", ble_name);
    lv_label_set_text(n, line);
    lv_obj_align(n, LV_ALIGN_CENTER, -80, -30);
    lv_obj_set_style_text_color(n, lv_color_white(), 0);

    lv_obj_t *p = lv_label_create(s_pair_panel);
    snprintf(line, sizeof(line), "POP: %s", pop);
    lv_label_set_text(p, line);
    lv_obj_align(p, LV_ALIGN_CENTER, -80, -6);
    lv_obj_set_style_text_color(p, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(p, 3, 0);

    /* 标准配网 QR: ESP BLE Provisioning App 扫码直连, 免手输 */
    char qrbuf[200];
    snprintf(qrbuf, sizeof(qrbuf),
             "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}",
             ble_name, pop);
    lv_obj_t *qr = lv_qrcode_create(s_pair_panel);
    lv_qrcode_set_size(qr, 110);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, qrbuf, (uint32_t)strlen(qrbuf));
    lv_obj_align(qr, LV_ALIGN_CENTER, 75, -12);

    lv_obj_t *h = lv_label_create(s_pair_panel);
    lv_label_set_text(h, "Scan with \"ESP BLE Provisioning\"\napp, or select device & enter POP");
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 62);
    lv_obj_set_style_text_color(h, lv_color_make(0x8a, 0x93, 0xa5), 0);
    bsp_display_unlock();
}

void ui_net_hide_pair(void)
{
    if (!bsp_display_lock(1000)) return;
    if (s_pair_panel) {
        lv_obj_del(s_pair_panel);
        s_pair_panel = NULL;
    }
    bsp_display_unlock();
}

/*
 * 状态文字点击: WiFi 未连接 (换了环境等) 时进入重新配网。
 * 3 秒内点两次才执行 (防误触); 重配 = 清凭据 + 重启落 BLE 配网页。
 */
static void on_net_status_click(lv_event_t *e)
{
    (void)e;
    if (pvc_net_state() == PVC_NET_ONLINE) return;   /* 在线不响应 */
    static int64_t s_last_us;
    int64_t now = esp_timer_get_time();
    if (now - s_last_us < 3000000) {
        toast_show("正在重启进入配网");
        pvc_net_reprovision();                        /* 不返回 */
    }
    s_last_us = now;
    toast_show("再点一次重新配网");
}

void ui_net_set_status(const char *txt)
{
    if (!bsp_display_lock(1000)) return;
    if (s_status_r) lv_label_set_text(s_status_r, txt);
    bsp_display_unlock();
}
