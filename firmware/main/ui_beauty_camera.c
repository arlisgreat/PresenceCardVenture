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
#include "pvc_voice.h"
#include "pvc_trace.h"
#include "esp_camera.h"
#include "img_converters.h"     /* fmt2jpg / jpg2rgb565 (esp32-camera) */

LV_FONT_DECLARE(pvc_font_cn16);

static const char *TAG = "ui_camera";

#define UI_W 320
#define UI_H 240
#define STATUS_H 24
#define BAR_H 60
#define FRAME_BYTES (UI_W * UI_H * 2)      /* RGB565 全屏 */

/* ---- Presence 设计 Tokens (《屏幕 UI 与交互量化设计规范》§1.1) ---- */
#define COL_GRASS   lv_color_make(0x3e, 0x7a, 0x3a)   /* 草地绿: 贴纸底 */
#define COL_SKY     lv_color_make(0x2f, 0x7d, 0xe0)   /* 天空蓝: 时间戳/选中 */
#define COL_PINK    lv_color_make(0xf7, 0xa8, 0xc8)   /* 糖果粉: 心动高亮 */
#define COL_CREAM   lv_color_make(0xf5, 0xd7, 0x6e)   /* 奶油黄: 星星本体 */
#define COL_LILAC   lv_color_make(0xc7, 0xb8, 0xee)   /* 丁香紫: 昵称贴纸底 */
#define COL_PAPER   lv_color_make(0xfd, 0xfb, 0xf5)   /* 纸白: 拍立得框/文本 */
#define COL_NIGHT   lv_color_make(0x1a, 0x1d, 0x1a)   /* 暗夜: 遮罩/深字 */

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
static void open_feed(void);
static void render_feed(void);
static void do_like(void);
static void spawn_like_float(int count);
static void open_publish_panel(void);
static lv_obj_t *panel_create(uint32_t h);
static void take_photo(void);
static void on_layer_pressed(lv_event_t *e);
static void on_layer_pressing(lv_event_t *e);
static void on_layer_released(lv_event_t *e);
static void layer_gesture_register(lv_obj_t *obj);
static void on_layer_long_press(lv_event_t *e);
static void icon_close(lv_obj_t *parent, int32_t x, int32_t y, lv_color_t c);
static void icon_send(lv_obj_t *parent, int32_t x, int32_t y, lv_color_t c);
static void icon_filter(lv_obj_t *parent, int32_t x, int32_t y, lv_color_t c);
static void icon_mirror(lv_obj_t *parent, int32_t x, int32_t y, lv_color_t c);
static void icon_envelope(lv_obj_t *parent, int32_t x, int32_t y, lv_color_t c);
static lv_obj_t *icon_line(lv_obj_t *parent, const lv_point_precise_t *pts,
                           uint32_t n, lv_color_t color, int32_t width);
static lv_obj_t *icon_dot(lv_obj_t *parent, int32_t x, int32_t y,
                          int32_t d, lv_color_t color);
static void spawn_star_at(int32_t x, int32_t y);

/* ================= 状态 ================= */
static lv_obj_t   *s_canvas;               /* 全屏预览画布 */
static uint16_t   *s_canvas_buf;           /* canvas 缓冲 (PSRAM) */

/* 渲染节流: 每 ~8 帧 (320ms) 刷新一次滤镜缩略图 */
static uint32_t s_frame_cnt = 0;

/* 滤镜 / 美颜 / 贴纸。扩展索引: 0..5 基础(hw2d 参数式), 6..9 CCD 机型(3D LUT) */
#define FILTER_CCD_BASE  HW2D_FILTER_MAX
#define FILTER_EXT_COUNT (HW2D_FILTER_MAX + CCD_CAM_COUNT)
static int s_filter = FILTER_CCD_BASE;      /* 默认 F100: 轻颗粒、低饱和、暗角 */
static int s_white = 16;                   /* 保留肤质, 只做轻微提亮 */
static int s_smooth = 12;
static int s_sticker = -1;                 /* -1 = 无贴纸 */
static bool s_mirror = true;               /* 自拍默认镜像; 可在工具栏切换 */

/* 三模式 (交付文档 v1.1 §1): 状态机实现见 feed 区之前 */
typedef enum { MODE_HOME = 0, MODE_CIRCLE, MODE_BIG, MODE_CAMERA } ui_mode_t;
static ui_mode_t s_mode = MODE_HOME;           /* 默认常驻屏, 相机下滑进入 */
static ui_mode_t s_mode_before_cam = MODE_HOME;
static void set_mode(ui_mode_t m);
static bool     s_sleeping = false;        /* 息屏模式 (§7) */
static int      s_sleep_missed = 0;        /* 息屏期间错过的新照片数 */
static void sleep_wake(void);

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
static uint16_t *s_review_raw_buf;          /* 快门时刻原始预览 */
static uint16_t *s_review_fx_buf;           /* 快门时刻滤镜预览 */
static lv_obj_t *s_review_fx_canvas;
static lv_obj_t *s_publish_controls;
static lv_obj_t *s_transfer_art;
static lv_obj_t *s_transfer_veil;
static lv_obj_t *s_transfer_dot;
static lv_obj_t *s_transfer_arc;
static lv_timer_t *s_publish_timer;
static bool s_publish_pending;
static bool s_transfer_waiting;
static bool s_transfer_closing;
static int64_t s_publish_open_us;
static int64_t s_transfer_started_us;
static int64_t s_transfer_close_us;
static volatile bool s_publish_queued;
static volatile bool s_publish_enqueue_ok;

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
    if (!s_canvas || s_publish_pending) return;
    if (s_mode != MODE_CAMERA) return;   /* 相机为下滑进入的子状态, 其余模式不渲染预览 */
    /* 声控拍照: 大声喊触发 (仅拍照模式; 触发即取走) */
    if (pvc_voice_take_trigger()) {
        PVC_EV("shutter via=voice");
        take_photo();
        return;
    }
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
        /* rot180: 装配方向补偿 (GC0308 寄存器翻转真机写不进, 软件反向写出) */
        hw2d_yuv_filter_rgb565_rot180_stat(s_canvas_buf, yuyv, UI_W * UI_H,
                                           &s_yuv_luts);
        if (s_mirror) hw2d_rgb565_hmirror(s_canvas_buf, UI_W, UI_H);
        if (thumb_due) {
            if (s_sticker >= 0 && s_face_rgb) {
                /* 人脸检测吃 RGB565: 恒等表转换一帧 (每 ~320ms);
                 * 同样 rot180, 人脸框坐标与旋转后的画面/照片一致 */
                hw2d_yuv_filter_rgb565_rot180(s_face_rgb, yuyv, UI_W * UI_H,
                                              &s_id_luts);
                if (s_mirror) hw2d_rgb565_hmirror(s_face_rgb, UI_W, UI_H);
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
    FILE *fp = fopen(path, "wb");
    bool ok = fp && fwrite(data, 1, len, fp) == len;
    if (fp) fclose(fp);
    if (!ok) ESP_LOGE(TAG, "write %s failed", path);
    return ok;
}

/*
 * JPEG 文件 -> RGB565 (小端)。利用 TJpgDec 的 1/2、1/4、1/8 降尺度解码:
 * 选能放进 maxw x maxh 的最小缩放档, 缩略图无需全尺寸解码 (快 4-10 倍)。
 * out 容量须 >= maxw x maxh x 2 字节; 实际尺寸经 ow/oh 出参返回。
 */
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

    uint32_t w = 0, h = 0;
    int s;
    if (!pvc_jpeg_dims(jbuf, (size_t)sz, &w, &h)) { PSRAM_FREE(jbuf); return -1; }
    for (s = 0; s <= 3; s++) {
        if ((w >> s) <= maxw && (h >> s) <= maxh) break;
    }
    if (s > 3) { PSRAM_FREE(jbuf); return -1; }
    uint32_t dw = w >> s, dh = h >> s;

    /* jpg2rgb565 输出小端 RGB565: 直接解码进目标缓冲, 无需中转/交换 */
    bool ok = jpg2rgb565(jbuf, (size_t)sz, (uint8_t *)out,
                         (esp_jpeg_image_scale_t)s);
    PSRAM_FREE(jbuf);
    if (!ok) return -1;
    *ow = dw;
    *oh = dh;
    return 0;
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

/* 拍后确认由 UI 通过队列交给 worker。旧逻辑固定等待 6s 且面板没有打开，
 * 是照片到 Web 的主要人为延迟；确认后现在立即进入编码/上传队列。 */
typedef struct {
    bool send;
} publish_choice_t;

static QueueHandle_t s_publish_choice_q;

static void photo_worker(void *arg)
{
    (void)arg;
    photo_job_t job;
    for (;;) {
        if (xQueueReceive(s_photo_q, &job, portMAX_DELAY) != pdTRUE) continue;
        int64_t t_deq = esp_timer_get_time();
        uint32_t pw = job.w, ph = job.h;

        /* YUV 域: Y 平面磨皮 -> Y/Cb/Cr 查表滤镜 (worker 自持表, 与预览无竞争)
         * -> 贴纸合成 (亮度提亮圆, 位置取快门时刻人脸框) */
        uint8_t *yuyv = (uint8_t *)job.snap;
        static hw2d_yuv_luts_t s_wk_luts;      /* worker 单任务专用 */
        /* 装配方向补偿: 先转 180 度, 后续人脸框/贴纸坐标即与预览一致 */
        hw2d_yuv_rot180(yuyv, pw * ph);
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
                mkdir("/sdcard/DCIM", 0755);
                write_file(path, s_wk_enc, alen);
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
            publish_choice_t choice = { 0 };
            if (s_publish_choice_q) {
                xQueueReceive(s_publish_choice_q, &choice, pdMS_TO_TICKS(20000));
            }
            PVC_EV("photo_encoded bytes=%u filter=%s beauty=%d",
                   (unsigned)jlen, filter_api_id(job.fid), job.white);
            if (choice.send) {
                esp_err_t up = pvc_net_enqueue_photo(
                    s_wk_enc, jlen, filter_api_id(job.fid), job.white,
                    "今天也在场", "小圈");
                esp_err_t msg = pvc_net_send_message_async("luna", "今天也在场");
                s_publish_enqueue_ok = (up == ESP_OK);
                s_publish_queued = true;
                PVC_EV("publish_confirmed upload=%d message=%d",
                       (int)s_publish_enqueue_ok, (int)(msg == ESP_OK));
            } else {
                PVC_EV("publish_cancelled ok=1");
            }
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

    if (s_sleeping) { sleep_wake(); return; }
    if (!s_canvas_buf || !s_photo_q) {
        toast_show("无画面");
        return;
    }
    if (s_publish_pending) return;
    int64_t t0 = esp_timer_get_time();
    pvc_sound_play(PVC_SND_SHUTTER);

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
    if (job.w == UI_W && job.h == UI_H && s_review_raw_buf && s_review_fx_buf) {
        const uint8_t *yuyv = (const uint8_t *)f->buf;
        hw2d_yuv_filter_rgb565_rot180(s_review_raw_buf, yuyv,
                                      UI_W * UI_H, &s_id_luts);
        hw2d_yuv_build_luts(&s_active_filter, &s_yuv_luts);
        hw2d_yuv_filter_rgb565_rot180(s_review_fx_buf, yuyv,
                                      UI_W * UI_H, &s_yuv_luts);
        if (s_mirror) {
            hw2d_rgb565_hmirror(s_review_raw_buf, UI_W, UI_H);
            hw2d_rgb565_hmirror(s_review_fx_buf, UI_W, UI_H);
        }
    }
    app_camera_release();
    job.grab_ms = (int)((esp_timer_get_time() - t0) / 1000);

    if (xQueueSend(s_photo_q, &job, 0) != pdTRUE) {
        snap_release(slot);
        PVC_EV("photo_drop reason=busy");
        toast_show("处理中, 稍候再拍");
        return;
    }
    /* 快门声和现场回声不能残留成下一次声控触发。 */
    pvc_voice_set_enabled(false);
    s_seq++;
    s_publish_pending = true;
    s_publish_queued = false;
    s_publish_enqueue_ok = false;
    open_publish_panel();
    /* 磨皮/滤镜/存卡/编码由 core1 worker 异步完成；发送确认后立即上传。 */
}

/* ================= 拍后确认 / 上传过渡 ================= */
static void anim_set_opa(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void style_large_touch_target(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(btn, 246, LV_STATE_PRESSED);
}

static void publish_close_now(void)
{
    if (s_publish_timer) {
        lv_timer_del(s_publish_timer);
        s_publish_timer = NULL;
    }
    close_panel();
    s_publish_pending = false;
    s_transfer_waiting = false;
    s_transfer_closing = false;
    s_review_fx_canvas = NULL;
    s_publish_controls = NULL;
    s_transfer_art = NULL;
    s_transfer_veil = NULL;
    s_transfer_dot = NULL;
    s_transfer_arc = NULL;
    /* 模式归属由调用处决定: 重拍/取消 -> 取景框; 发布成功 -> 拍照前模式 */
}

static void publish_finish(bool send)
{
    if (!s_publish_pending || s_transfer_waiting) return;
    publish_choice_t choice = { .send = send };
    if (s_publish_choice_q) xQueueSend(s_publish_choice_q, &choice, 0);

    if (!send) {
        publish_close_now();
        set_mode(MODE_CAMERA);     /* 重拍/取消: 回取景框 (规范 §5) */
        return;
    }

    s_transfer_waiting = true;
    s_transfer_closing = false;
    s_transfer_started_us = esp_timer_get_time();
    s_publish_queued = false;
    s_publish_enqueue_ok = false;
    if (s_publish_controls) lv_obj_add_flag(s_publish_controls, LV_OBJ_FLAG_HIDDEN);
    if (s_transfer_art) lv_obj_clear_flag(s_transfer_art, LV_OBJ_FLAG_HIDDEN);
    if (s_review_fx_canvas) lv_obj_set_style_opa(s_review_fx_canvas, LV_OPA_COVER, 0);
    PVC_EV("publish_transition started=1");
}

static void on_publish_click(lv_event_t *e)
{
    publish_finish((bool)(intptr_t)lv_event_get_user_data(e));
}

static void publish_timer_cb(lv_timer_t *t)
{
    int64_t now = esp_timer_get_time();
    if (!s_publish_pending) {
        lv_timer_del(t);
        s_publish_timer = NULL;
        return;
    }

    if (!s_transfer_waiting) {
        if (now - s_publish_open_us > 20000000) publish_finish(false);
        return;
    }

    int elapsed = (int)((now - s_transfer_started_us) / 1000);
    if (s_transfer_arc) {
        int start = (elapsed / 5) % 360;
        lv_arc_set_angles(s_transfer_arc, start, start + 245);
    }
    if (s_transfer_dot) {
        int tri = (elapsed / 8) % 64;
        if (tri > 32) tri = 64 - tri;
        lv_obj_set_style_opa(s_transfer_dot, (lv_opa_t)(150 + tri * 3), 0);
    }
    if (s_transfer_veil) {
        int veil = elapsed < 1800 ? (elapsed * 70 / 1800) : 70;
        lv_obj_set_style_bg_opa(s_transfer_veil, (lv_opa_t)veil, 0);
    }

    bool sent = s_publish_queued && s_publish_enqueue_ok && pvc_net_synced();
    bool failed = s_publish_queued && !s_publish_enqueue_ok;
    bool timed_out = elapsed >= 9000;
    if (!s_transfer_closing && elapsed >= 1200 && (sent || failed || timed_out)) {
        s_transfer_closing = true;
        s_transfer_close_us = now;
        if (sent) pvc_sound_play(PVC_SND_DING);
        PVC_EV("publish_transition done=%d timeout=%d", (int)sent, (int)timed_out);
    }

    if (s_transfer_closing) {
        int close_ms = (int)((now - s_transfer_close_us) / 1000);
        if (close_ms >= 460) {
            publish_close_now();
            /* 发布成功 -> 回拍照前模式; 失败/超时 -> 回取景框重试 */
            set_mode(sent ? s_mode_before_cam : MODE_CAMERA);
            return;
        }
        if (s_panel) {
            int opa = 255 - close_ms * 255 / 460;
            lv_obj_set_style_opa(s_panel, (lv_opa_t)opa, 0);
        }
    }
}

static void open_publish_panel(void)
{
    if (!s_review_raw_buf || !s_review_fx_buf) {
        publish_choice_t choice = { .send = true };
        if (s_publish_choice_q) xQueueSend(s_publish_choice_q, &choice, 0);
        s_publish_pending = false;
        /* 极低内存降级为直接发送时，仍留在相机页，重新开启声控。 */
        pvc_voice_set_enabled(s_mode == MODE_CAMERA);
        return;
    }
    if (s_publish_choice_q) xQueueReset(s_publish_choice_q);
    close_panel();

    lv_obj_t *p = lv_obj_create(lv_scr_act());
    s_panel = p;
    lv_obj_set_size(p, UI_W, UI_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_black(), 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_shadow_width(p, 0, 0);
    /* 定格预览手势: 上滑=重拍 (规范 §5); 关滚动保手势 */
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(p, on_layer_long_press, LV_EVENT_LONG_PRESSED, NULL);
    layer_gesture_register(p);

    lv_obj_t *raw = lv_canvas_create(p);
    lv_canvas_set_buffer(raw, s_review_raw_buf, UI_W, UI_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(raw, 0, 0);
    lv_obj_remove_flag(raw, LV_OBJ_FLAG_CLICKABLE);

    s_review_fx_canvas = lv_canvas_create(p);
    lv_canvas_set_buffer(s_review_fx_canvas, s_review_fx_buf,
                         UI_W, UI_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_review_fx_canvas, 0, 0);
    lv_obj_set_style_opa(s_review_fx_canvas, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_review_fx_canvas, LV_OBJ_FLAG_CLICKABLE);

    /* 原片先出现，F100 胶片质感在 0.9s 内渐显。 */
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, s_review_fx_canvas);
    lv_anim_set_exec_cb(&fade, anim_set_opa);
    lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade, 900);
    lv_anim_set_delay(&fade, 120);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_in_out);
    lv_anim_start(&fade);

    s_transfer_veil = lv_obj_create(p);
    lv_obj_set_size(s_transfer_veil, UI_W, UI_H);
    lv_obj_set_pos(s_transfer_veil, 0, 0);
    lv_obj_set_style_bg_color(s_transfer_veil, lv_color_make(0xf4, 0xee, 0xe4), 0);
    lv_obj_set_style_bg_opa(s_transfer_veil, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_transfer_veil, 0, 0);
    lv_obj_set_style_radius(s_transfer_veil, 0, 0);
    lv_obj_set_style_pad_all(s_transfer_veil, 0, 0);
    lv_obj_remove_flag(s_transfer_veil, LV_OBJ_FLAG_CLICKABLE);

    /* 发送等待：细轨道 + 信封，只有暖红色圆点在呼吸。 */
    s_transfer_art = lv_obj_create(p);
    lv_obj_set_size(s_transfer_art, 142, 76);
    lv_obj_align(s_transfer_art, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_bg_color(s_transfer_art, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_transfer_art, LV_OPA_40, 0);
    lv_obj_set_style_border_width(s_transfer_art, 0, 0);
    lv_obj_set_style_radius(s_transfer_art, 0, 0);
    lv_obj_set_style_pad_all(s_transfer_art, 0, 0);
    lv_obj_remove_flag(s_transfer_art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_transfer_art, LV_OBJ_FLAG_HIDDEN);

    s_transfer_arc = lv_arc_create(s_transfer_art);
    lv_obj_set_size(s_transfer_arc, 44, 44);
    lv_obj_set_pos(s_transfer_arc, 12, 16);
    lv_arc_set_bg_angles(s_transfer_arc, 0, 360);
    lv_arc_set_angles(s_transfer_arc, 0, 245);
    lv_obj_set_style_arc_width(s_transfer_arc, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_transfer_arc, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_transfer_arc, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_transfer_arc, 2, LV_PART_INDICATOR);
    lv_obj_remove_style(s_transfer_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_transfer_arc, LV_OBJ_FLAG_CLICKABLE);

    s_transfer_dot = lv_obj_create(s_transfer_art);
    lv_obj_set_size(s_transfer_dot, 7, 7);
    lv_obj_set_pos(s_transfer_dot, 47, 19);
    lv_obj_set_style_bg_color(s_transfer_dot, lv_color_make(0xbd, 0x4a, 0x3a), 0);
    lv_obj_set_style_border_width(s_transfer_dot, 0, 0);
    lv_obj_set_style_radius(s_transfer_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_transfer_dot, 0, 0);
    lv_obj_remove_flag(s_transfer_dot, LV_OBJ_FLAG_CLICKABLE);

    icon_envelope(s_transfer_art, 94, 27, lv_color_white());

    /* 底部控件视觉很小，真实点击区域各 136x72。 */
    s_publish_controls = lv_obj_create(p);
    lv_obj_set_size(s_publish_controls, UI_W, 72);
    lv_obj_set_pos(s_publish_controls, 0, UI_H - 72);
    lv_obj_set_style_bg_color(s_publish_controls, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_publish_controls, LV_OPA_40, 0);
    lv_obj_set_style_border_width(s_publish_controls, 0, 0);
    lv_obj_set_style_radius(s_publish_controls, 0, 0);
    lv_obj_set_style_pad_all(s_publish_controls, 0, 0);
    lv_obj_remove_flag(s_publish_controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_publish_controls, on_layer_long_press,
                        LV_EVENT_LONG_PRESSED, NULL);
    layer_gesture_register(s_publish_controls);

    lv_obj_t *cancel = lv_btn_create(s_publish_controls);
    lv_obj_set_size(cancel, 136, 72);
    lv_obj_set_pos(cancel, 0, 0);
    style_large_touch_target(cancel);
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(cancel, on_publish_click, LV_EVENT_CLICKED, (void *)0);
    /* 重拍药丸 (规范画面 2): 90x36 纸白 R18, 深色 × */
    lv_obj_t *cancel_pill = lv_obj_create(cancel);
    lv_obj_set_size(cancel_pill, 90, 36);
    lv_obj_center(cancel_pill);
    lv_obj_set_style_bg_color(cancel_pill, COL_PAPER, 0);
    lv_obj_set_style_bg_opa(cancel_pill, LV_OPA_80, 0);
    lv_obj_set_style_border_width(cancel_pill, 0, 0);
    lv_obj_set_style_radius(cancel_pill, 18, 0);
    lv_obj_set_style_pad_all(cancel_pill, 0, 0);
    lv_obj_remove_flag(cancel_pill, LV_OBJ_FLAG_CLICKABLE);
    icon_close(cancel_pill, 34, 7, COL_NIGHT);

    lv_obj_t *send = lv_btn_create(s_publish_controls);
    lv_obj_set_size(send, 136, 72);
    lv_obj_set_pos(send, UI_W - 136, 0);
    style_large_touch_target(send);
    lv_obj_add_flag(send, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(send, on_publish_click, LV_EVENT_CLICKED, (void *)1);
    /* 发布药丸 (规范画面 2): 天空蓝 R18, 白色上升箭头 */
    lv_obj_t *send_pill = lv_obj_create(send);
    lv_obj_set_size(send_pill, 90, 36);
    lv_obj_center(send_pill);
    lv_obj_set_style_bg_color(send_pill, COL_SKY, 0);
    lv_obj_set_style_bg_opa(send_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(send_pill, 0, 0);
    lv_obj_set_style_radius(send_pill, 18, 0);
    lv_obj_set_style_pad_all(send_pill, 0, 0);
    lv_obj_remove_flag(send_pill, LV_OBJ_FLAG_CLICKABLE);
    icon_send(send_pill, 34, 5, COL_PAPER);

    lv_obj_move_foreground(p);
    s_publish_open_us = esp_timer_get_time();
    s_publish_timer = lv_timer_create(publish_timer_cb, 50, NULL);
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
/* 滤镜指示 tag (规范画面 2): 顶部居中黑底 0.4 R10, 切换时短暂出现 1.5s */
static lv_obj_t *s_filter_tag = NULL;
static lv_timer_t *s_filter_tag_timer = NULL;
static const char *filter_name(int f);

static void filter_tag_hide_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    s_filter_tag_timer = NULL;
    if (s_filter_tag) lv_obj_add_flag(s_filter_tag, LV_OBJ_FLAG_HIDDEN);
}

static void show_filter_tag(void)
{
    if (!s_filter_tag) return;
    lv_label_set_text(s_filter_tag, filter_name(s_filter));
    lv_obj_clear_flag(s_filter_tag, LV_OBJ_FLAG_HIDDEN);
    if (s_filter_tag_timer) lv_timer_del(s_filter_tag_timer);
    s_filter_tag_timer = lv_timer_create(filter_tag_hide_cb, 1500, NULL);
}

static void on_filter_click(lv_event_t *e)
{
    s_filter = (int)(intptr_t)lv_event_get_user_data(e);
    update_active_filter();
    s_thumb_dirty = true;
    close_panel();
    update_status();
    show_filter_tag();
}

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
    if (load_jpg_565(s_album_paths[item], tmp, UI_W, UI_H, &w, &h) != 0) {
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

    d = opendir("/sdcard/DCIM");
    if (!d) return;
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
    s_album_n = n;

    for (int i = 0; i < MAX_ALBUM_ITEMS; i++) {
        lv_obj_t *cell = s_album_grid[i];
        if (!cell) continue;
        if (i < n && s_album_bufs[i]) {
            uint32_t w, h;
            /* 1/4 档解码 (VGA -> 160x120) 再缩到 96x72, 免全尺寸解码 */
            uint16_t *tmp = PSRAM_MALLOC(160 * 120 * 2);
            if (tmp) {
                if (load_jpg_565(s_album_paths[i], tmp, 160, 120, &w, &h) == 0) {
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

/* ================= 轻回应星星 (规范 §3.3-B: Tap 散 ⭐ + 发 like) ================= */
static void star_cb_y(void *o, int32_t v)    { lv_obj_set_y((lv_obj_t *)o, v); }
static void star_cb_opa(void *o, int32_t v)  { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
static void star_cb_zoom(void *o, int32_t v) { lv_obj_set_style_transform_zoom((lv_obj_t *)o, (uint16_t)v, 0); }

/* 四点像素星 (12->20px, 奶油黄, 上飘 40px 同时放大淡出, 1000ms ease-out) */
static const lv_point_precise_t P_STAR_V[] = {{8, 0}, {8, 16}};
static const lv_point_precise_t P_STAR_H[] = {{0, 8}, {16, 8}};
static void spawn_star_at(int32_t x, int32_t y)
{
    lv_obj_t *g = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g, 17, 17);
    lv_obj_set_pos(g, x - 8, y - 8);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_set_style_radius(g, 0, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *v = icon_line(g, P_STAR_V, 2, COL_CREAM, 2);
    lv_obj_set_pos(v, 8, 0);
    lv_obj_t *h = icon_line(g, P_STAR_H, 2, COL_CREAM, 2);
    lv_obj_set_pos(h, 0, 8);
    icon_dot(g, 5, 5, 7, COL_CREAM);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g);
    lv_anim_set_exec_cb(&a, star_cb_y);
    lv_anim_set_values(&a, y - 8, y - 48);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, g);
    lv_anim_set_exec_cb(&a, star_cb_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, g);
    lv_anim_set_exec_cb(&a, star_cb_zoom);
    lv_anim_set_values(&a, 205, 358);   /* 0.8x -> 1.4x */
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_obj_delete_delayed(g, 1080);
}

/* 被赞通知 (云端轮询发现本人照片新增 like): 连散数颗星 */
static void spawn_like_float(int count)
{
    if (count > 3) count = 3;
    for (int i = 0; i < count; i++) {
        spawn_star_at(120 + i * 40, 150 - (i % 2) * 24);
    }
}

/* ================= 三模式状态机 (交付文档 v1.1 §1-§3) =================
 * MODE_HOME   常驻主图/足迹 (默认, 离线完整可用)
 * MODE_CIRCLE 小圈流 (朋友照片)
 * MODE_BIG    大圈 (订阅精选; 无订阅时空态)
 * MODE_CAMERA 拍照 (下滑进入; 取景器是全屏子状态, 不是默认屏)
 */
/* feed 层对象前置 (状态机引用; 函数见下方 feed 区) */
static lv_obj_t *s_feed_panel = NULL;
static lv_obj_t *s_feed_canvas = NULL;
static uint16_t *s_feed_buf = NULL;           /* 320x240 RGB565 (PSRAM) */
static lv_obj_t *s_feed_author, *s_feed_caption, *s_feed_counter;
static lv_obj_t *s_feed_circle_btn = NULL;    /* 全部/小圈 切换 */
static pvc_feed_item_t s_feed_items[PVC_FEED_MAX];
static int s_feed_n = 0, s_feed_idx = 0;
static bool s_feed_circle_only = false;   /* true = 只看小圈内容 */
static bool s_feed_auto = false;          /* 到达仪式自动展示中 (交互即取消) */
static lv_timer_t *s_arrival_timer = NULL;
static int  feed_snapshot_filtered(void);
static void on_feed_next(lv_event_t *e);
static void on_feed_prev(lv_event_t *e);
static void arrival_hide_cb(lv_timer_t *t);
static bool     s_feed_big = false;        /* feed 层当前按大圈筛选 */
static lv_obj_t *s_bar = NULL;             /* 相机底部控制条 (CAMERA 可见) */
static lv_obj_t *s_feed_hot = NULL;        /* 相机顶部小圈快捷 (CAMERA 可见) */
static lv_obj_t *s_home_canvas = NULL;     /* 常驻主图/足迹画布 */
static uint16_t *s_home_buf = NULL;
static lv_obj_t *s_home_hint = NULL;       /* 空态提示 */
#define FOOT_MAX 12
static char  s_foot_paths[FOOT_MAX][64];
static int   s_foot_n = 0, s_foot_idx = 0;
static lv_obj_t *s_picker = NULL;          /* 三模式选择盘 */
static lv_obj_t *s_picker_card[3];
static lv_timer_t *s_picker_idle = NULL;
static lv_obj_t *s_sheet = NULL;           /* 轻系统页 */
static lv_obj_t *s_mode_tag = NULL;        /* 模式名贴纸 */
static lv_timer_t *s_mode_tag_timer = NULL;

static void set_mode(ui_mode_t m);
static void show_filter_tag(void);
static void ui_sheet_open(void);

/* ---- 足迹扫描: SD DCIM 最新 FOOT_MAX 张 (离线数据源) ---- */
static void scan_footprints(void)
{
    s_foot_n = 0;
    DIR *d = opendir("/sdcard/DCIM");
    if (!d) return;
    /* 文件名为时间序 (boot 计数前缀), 直接收集后按名字倒序 */
    struct dirent *e;
    while ((e = readdir(d)) && s_foot_n < FOOT_MAX) {
        const char *nm = e->d_name;
        size_t ln = strlen(nm);
        if (ln < 5 || strcmp(nm + ln - 4, ".jpg") != 0) continue;
        /* 简单插入保持名字降序 (名字即时间序) */
        /* 文件名长度截断到 50 (d_name 理论 255, 路径缓冲 64) */
        char safe[51];
        strncpy(safe, nm, sizeof(safe) - 1);
        safe[sizeof(safe) - 1] = '\0';
        int i = s_foot_n++;
        snprintf(s_foot_paths[i], 64, "/sdcard/DCIM/%s", safe);
        while (i > 0 && strcmp(s_foot_paths[i - 1], s_foot_paths[i]) < 0) {
            char tmp[64];
            memcpy(tmp, s_foot_paths[i - 1], 64);
            memcpy(s_foot_paths[i - 1], s_foot_paths[i], 64);
            memcpy(s_foot_paths[i], tmp, 64);
            i--;
        }
    }
    closedir(d);
}

static void home_show(int idx)
{
    if (!s_home_buf || !s_home_canvas) return;
    if (s_foot_n == 0) {
        if (s_home_hint) lv_obj_clear_flag(s_home_hint, LV_OBJ_FLAG_HIDDEN);
        hw2d_fill(s_home_buf, UI_W * UI_H, rgb565(0x1a, 0x1d, 0x1a));
        lv_obj_invalidate(s_home_canvas);
        return;
    }
    if (s_home_hint) lv_obj_add_flag(s_home_hint, LV_OBJ_FLAG_HIDDEN);
    if (idx < 0) idx = 0;
    if (idx >= s_foot_n) idx = s_foot_n - 1;
    s_foot_idx = idx;
    uint32_t w = 0, h = 0;
    if (load_jpg_565(s_foot_paths[idx], s_home_buf, UI_W, UI_H, &w, &h) != 0 ||
        w != UI_W || h != UI_H) {
        hw2d_fill(s_home_buf, UI_W * UI_H, rgb565(0x1a, 0x1d, 0x1a));
    }
    lv_obj_invalidate(s_home_canvas);
}

/* ---- 模式名贴纸 (切模式时顶部浮现 1.5s, 规范 §3) ---- */
static void mode_tag_hide_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    s_mode_tag_timer = NULL;
    if (s_mode_tag) lv_obj_add_flag(s_mode_tag, LV_OBJ_FLAG_HIDDEN);
}
static void show_mode_tag(const char *txt)
{
    if (!s_mode_tag) return;
    lv_label_set_text(s_mode_tag, txt);
    lv_obj_clear_flag(s_mode_tag, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_mode_tag);
    if (s_mode_tag_timer) lv_timer_del(s_mode_tag_timer);
    s_mode_tag_timer = lv_timer_create(mode_tag_hide_cb, 1500, NULL);
}

/* ---- 模式切换 ---- */
static void set_mode(ui_mode_t m)
{
    if (m != MODE_CAMERA) s_mode_before_cam = m;
    s_mode = m;
    bool cam = (m == MODE_CAMERA);
    pvc_voice_set_enabled(cam && !s_publish_pending);
    if (s_canvas)   lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    if (!cam && s_canvas) { /* 非相机时预览停渲染 (preview_timer_cb 守卫) */ }
    if (cam && s_canvas)    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    if (s_bar)      cam ? lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN)
                        : lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    if (s_feed_hot) cam ? lv_obj_clear_flag(s_feed_hot, LV_OBJ_FLAG_HIDDEN)
                        : lv_obj_add_flag(s_feed_hot, LV_OBJ_FLAG_HIDDEN);
    if (s_status_r) cam ? lv_obj_clear_flag(s_status_r, LV_OBJ_FLAG_HIDDEN)
                        : lv_obj_add_flag(s_status_r, LV_OBJ_FLAG_HIDDEN);
    if (!cam && s_filter_tag) lv_obj_add_flag(s_filter_tag, LV_OBJ_FLAG_HIDDEN);
    if (s_home_canvas) {
        if (m == MODE_HOME) lv_obj_clear_flag(s_home_canvas, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_home_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    if (m == MODE_HOME) {
        scan_footprints();
        home_show(s_foot_idx);
    }
    if (m == MODE_CIRCLE || m == MODE_BIG) {
        s_feed_big = (m == MODE_BIG);
        s_feed_circle_only = false;
        open_feed();
        if (s_feed_n == 0) {
            /* 空态回退常驻 (小圈无照片 / 大圈未订阅) */
            if (m == MODE_BIG) toast_show("大圈在 App 端订阅");
            s_feed_big = false;
            s_mode = m = MODE_HOME;
            if (s_home_canvas) lv_obj_clear_flag(s_home_canvas, LV_OBJ_FLAG_HIDDEN);
            scan_footprints();
            home_show(s_foot_idx);
        }
    } else if (s_feed_panel && !lv_obj_has_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN)
               && !s_feed_auto) {
        lv_obj_add_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN);
    }
    static const char *const names[] = { "常驻", "小圈", "大圈", "拍照" };
    show_mode_tag(names[m]);
    PVC_EV("mode m=%d", (int)m);
}

/* ---- 取景框左右轻划切滤镜 (规范 §5) ---- */
static void filter_step(int d)
{
    s_filter = (s_filter + FILTER_EXT_COUNT + d) % FILTER_EXT_COUNT;
    update_active_filter();
    s_thumb_dirty = true;
    update_status();
    show_filter_tag();
}

/* ---- 息屏 / 唤醒 (规范 §7) ---- */
static void sleep_enter(void)
{
    if (s_sleeping) return;
    s_sleeping = true;
    s_sleep_missed = 0;
    bsp_display_backlight_off();   /* 800ms 渐暗待 BSP 亮度级支持后补 */
    PVC_EV("sleep enter");
}
static void sleep_wake(void)
{
    if (!s_sleeping) return;
    s_sleeping = false;
    bsp_display_backlight_on();
    lv_display_trigger_activity(NULL);
    PVC_EV("sleep wake missed=%d", s_sleep_missed);
    /* 错过的照片: 最新一张先闪现 20s */
    if (s_sleep_missed > 0) {
        s_sleep_missed = 0;
        s_feed_big = false;
        s_feed_circle_only = false;
        s_feed_n = feed_snapshot_filtered();
        if (s_feed_n > 0 && s_feed_panel && s_feed_buf) {
            s_feed_idx = 0;
            s_feed_auto = true;
            lv_obj_set_style_opa(s_feed_panel, LV_OPA_COVER, 0);
            lv_obj_clear_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_feed_panel);
            render_feed();
            if (s_arrival_timer) lv_timer_del(s_arrival_timer);
            s_arrival_timer = lv_timer_create(arrival_hide_cb, 20000, NULL);
        }
    }
}

/* ---- 全局手势路由 (规范 §2: 各模式根层注册, 回调统一进这里) ---- */
static void gesture_dispatch(lv_dir_t dir)
{
    PVC_EV("gesture dir=%d mode=%d", (int)dir, (int)s_mode);
    if (s_sleeping) { sleep_wake(); return; }
    if (s_picker) return;                       /* 选择盘打开期间不导航 */
    if (s_sheet && !lv_obj_has_flag(s_sheet, LV_OBJ_FLAG_HIDDEN)) {
        /* 系统页打开中: 下滑收起 */
        if (dir == LV_DIR_BOTTOM) lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (s_publish_pending) {                    /* 定格预览: 上滑=重拍 */
        if (dir == LV_DIR_TOP) publish_finish(false);
        return;
    }
    switch (s_mode) {
    case MODE_CAMERA:
        if (dir == LV_DIR_LEFT)       filter_step(+1);
        else if (dir == LV_DIR_RIGHT) filter_step(-1);
        else if (dir == LV_DIR_TOP)   set_mode(s_mode_before_cam);
        break;
    case MODE_HOME:
        if (dir == LV_DIR_BOTTOM)     set_mode(MODE_CAMERA);
        else if (dir == LV_DIR_TOP)   ui_sheet_open();
        else if (dir == LV_DIR_LEFT)  home_show(s_foot_idx + 1);
        else if (dir == LV_DIR_RIGHT) home_show(s_foot_idx - 1);
        break;
    case MODE_CIRCLE:
    case MODE_BIG:
        if (dir == LV_DIR_BOTTOM)     set_mode(MODE_CAMERA);
        else if (dir == LV_DIR_TOP)   ui_sheet_open();
        else if (dir == LV_DIR_LEFT)  on_feed_next(NULL);
        else if (dir == LV_DIR_RIGHT) {
            if (s_feed_idx == 0) set_mode(MODE_HOME);
            else on_feed_prev(NULL);
        }
        break;
    }
}

/* 常驻屏单击: 息屏唤醒; 否则按压处散星 (纯视觉, 本地足迹无 photo_id 不发 like) */
static void on_home_click(lv_event_t *e)
{
    (void)e;
    if (s_sleeping) { sleep_wake(); return; }
    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        spawn_star_at(p.x, p.y);
    }
}

/* ---- 触摸手动诊断 (LVGL 9: 内置 GESTURE 只识别 scroll; 手动累计位移) ----
 * 按帧累计: PRESSING 回调里读 lv_indev_get_vect 拿这一帧的增量, 累计到 RELEASED
 * 判定。注意: LVGL 内部 gesture_sum 仅当按下位置不变才累计, 所以此处自行累计。 */
#define GEST_MIN 30          /* 位移阈值 px (规范 40, 触摸屏偏小降为 30) */
#define GEST_MAX_OTHER 60    /* 另一轴上限 px (规范 30 -> 60 容错) */
static int  s_touch_dx = 0, s_touch_dy = 0;
static void touch_diag_reset(void) { s_touch_dx = 0; s_touch_dy = 0; }

static void touch_diag_read(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);      /* 本帧增量 */
    s_touch_dx += v.x;
    s_touch_dy += v.y;
}
static void touch_diag_release(lv_event_t *e)
{
    (void)e;
    int dx = s_touch_dx, dy = s_touch_dy;
    touch_diag_reset();
    if (dx == 0 && dy == 0) return;
    /* 阈值判定: 主轴 >= GEST_MIN, 副轴 <= GEST_MAX_OTHER */
    lv_dir_t dir = LV_DIR_NONE;
    if (abs(dx) > abs(dy)) {
        if (abs(dy) <= GEST_MAX_OTHER && abs(dx) >= GEST_MIN)
            dir = dx > 0 ? LV_DIR_RIGHT : LV_DIR_LEFT;
    } else {
        if (abs(dx) <= GEST_MAX_OTHER && abs(dy) >= GEST_MIN)
            dir = dy > 0 ? LV_DIR_BOTTOM : LV_DIR_TOP;
    }
    if (dir != LV_DIR_NONE) gesture_dispatch(dir);
}
static void on_layer_pressed(lv_event_t *e) { (void)e; touch_diag_reset(); }
static void on_layer_pressing(lv_event_t *e) { (void)e; touch_diag_read(e); }
static void on_layer_released(lv_event_t *e) { touch_diag_release(e); }

/* 所有手势层的回调一次性注册 */
static void layer_gesture_register(lv_obj_t *obj)
{
    lv_obj_add_event_cb(obj, on_layer_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, on_layer_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(obj, on_layer_released, LV_EVENT_RELEASED, NULL);
}

/* ---- 三模式选择盘 (规范 §3: 长按 1.5s 呼出) ---- */
static void picker_close(void)
{
    if (s_picker_idle) { lv_timer_del(s_picker_idle); s_picker_idle = NULL; }
    if (s_picker) { lv_obj_del(s_picker); s_picker = NULL; }
}
static void picker_idle_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    s_picker_idle = NULL;
    picker_close();      /* 3s 无操作自动取消 */
}
static void picker_pick_cb(lv_event_t *e)
{
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    picker_close();
    set_mode((ui_mode_t)m);   /* 选择盘淡出并入 200ms 由 LVGL 默认删除动画省略 */
}
static void picker_mask_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) picker_close();
}
static void picker_open(void)
{
    if (s_picker || s_sleeping || s_publish_pending) return;
    if (s_sheet) { lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN); }
    s_picker = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_picker, UI_W, UI_H);
    lv_obj_set_pos(s_picker, 0, 0);
    lv_obj_set_style_bg_color(s_picker, COL_NIGHT, 0);
    lv_obj_set_style_bg_opa(s_picker, LV_OPA_40, 0);   /* 背景压暗 40% */
    lv_obj_set_style_border_width(s_picker, 0, 0);
    lv_obj_set_style_pad_all(s_picker, 0, 0);
    lv_obj_set_style_radius(s_picker, 0, 0);
    lv_obj_add_event_cb(s_picker, picker_mask_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_picker);

    static const char *const names[3] = { "常驻", "小圈", "大圈" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *card = lv_btn_create(s_picker);
        lv_obj_set_size(card, 88, 88);
        lv_obj_set_pos(card, (UI_W - (88 * 3 + 12 * 2)) / 2 + i * (88 + 12),
                       108 - 44);
        lv_obj_set_style_bg_color(card, COL_PAPER, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_add_event_cb(card, picker_pick_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        /* 当前模式: 2px 奶油黄描边 + scale 1.05 */
        bool cur = (int)s_mode == i;
        lv_obj_set_style_border_width(card, cur ? 2 : 0, 0);
        lv_obj_set_style_border_color(card, COL_CREAM, 0);
        if (cur) lv_obj_set_style_transform_scale(card, 268, 0);  /* ~1.05 */
        lv_obj_t *l = lv_label_create(card);
        lv_label_set_text(l, names[i]);
        lv_obj_set_style_text_color(l, COL_NIGHT, 0);
        lv_obj_center(l);
        s_picker_card[i] = card;
    }
    s_picker_idle = lv_timer_create(picker_idle_cb, 3000, NULL);
}
static void on_layer_long_press(lv_event_t *e)
{
    (void)e;
    if (s_mode == MODE_CAMERA || s_publish_pending) return;  /* 拍照/预览态不呼出 */
    picker_open();
}

/* ---- 轻系统页 (规范 §6: 上滑呼出, 高 172px 底部伸出) ---- */
static void sheet_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_sheet) lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
}
static void sheet_sleep_cb(lv_event_t *e)
{
    (void)e;
    if (s_sheet) lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
    sleep_enter();
}
static void sheet_skin_cb(lv_event_t *e)
{
    (void)e;
    toast_show("皮肤套件后续上线");
}
static void ui_sheet_open(void)
{
    if (!s_sheet || s_sleeping || s_publish_pending) return;
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_sheet);
}
static void sheet_build(lv_obj_t *scr)
{
    s_sheet = lv_obj_create(scr);
    lv_obj_set_size(s_sheet, UI_W, 172);
    lv_obj_set_pos(s_sheet, 0, UI_H - 172);
    lv_obj_set_style_bg_color(s_sheet, COL_NIGHT, 0);
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_sheet, 0, 0);
    lv_obj_set_style_radius(s_sheet, 0, 0);
    lv_obj_set_style_pad_all(s_sheet, 0, 0);
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_sheet, on_layer_long_press, LV_EVENT_LONG_PRESSED, NULL);
    layer_gesture_register(s_sheet);

    /* 状态栏: WiFi + 固件版本 */
    lv_obj_t *top = lv_label_create(s_sheet);
    lv_label_set_text(top, "Presence Card OS  v" FW_VERSION);
    lv_obj_set_style_text_color(top, COL_PAPER, 0);
    lv_obj_set_pos(top, 10, 6);

    /* 皮肤三卡 (P1 占位) */
    static const char *const skins[3] = { "草地手贴", "纯净胶片", "夜间床头" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *sk = lv_btn_create(s_sheet);
        lv_obj_set_size(sk, 88, 64);
        lv_obj_set_pos(sk, 12 + i * (88 + 14), 30);
        lv_obj_set_style_bg_color(sk, lv_color_make(0x2a, 0x2e, 0x2a), 0);
        lv_obj_set_style_radius(sk, 8, 0);
        lv_obj_set_style_border_width(sk, 2, 0);
        lv_obj_set_style_border_color(sk, COL_PAPER, 0);
        lv_obj_set_style_shadow_width(sk, 0, 0);
        lv_obj_add_flag(sk, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(sk, sheet_skin_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(sk);
        lv_label_set_text(l, skins[i]);
        lv_obj_set_style_text_color(l, COL_PAPER, 0);
        lv_obj_center(l);
    }

    /* 息屏按钮 */
    lv_obj_t *slp = lv_btn_create(s_sheet);
    lv_obj_set_size(slp, 88, 32);
    lv_obj_set_pos(slp, 12, 112);
    lv_obj_set_style_bg_color(slp, COL_SKY, 0);
    lv_obj_set_style_radius(slp, 16, 0);
    lv_obj_set_style_shadow_width(slp, 0, 0);
    lv_obj_add_flag(slp, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(slp, sheet_sleep_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(slp);
    lv_label_set_text(sl, "息屏");
    lv_obj_set_style_text_color(sl, COL_PAPER, 0);
    lv_obj_center(sl);

    /* 收起按钮 (右下) */
    lv_obj_t *cls = lv_btn_create(s_sheet);
    lv_obj_set_size(cls, 88, 32);
    lv_obj_set_pos(cls, UI_W - 100, 112);
    lv_obj_set_style_bg_color(cls, lv_color_make(0x2a, 0x2e, 0x2a), 0);
    lv_obj_set_style_radius(cls, 16, 0);
    lv_obj_set_style_shadow_width(cls, 0, 0);
    lv_obj_add_flag(cls, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(cls, sheet_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cls);
    lv_label_set_text(cl, "收起");
    lv_obj_set_style_text_color(cl, COL_PAPER, 0);
    lv_obj_center(cl);
}


/* feed 对象声明已前移至状态机区; 此处为函数实现 */

/* 拉取缓存快照; 小圈模式下只保留带小圈标记的条目 */
static int feed_snapshot_filtered(void)
{
    pvc_feed_item_t all[PVC_FEED_MAX];
    int n = pvc_feed_snapshot(all, PVC_FEED_MAX);
    if (!s_feed_circle_only) {
        memcpy(s_feed_items, all, sizeof(pvc_feed_item_t) * (size_t)n);
        return n;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (all[i].circle[0]) s_feed_items[m++] = all[i];
    }
    return m;
}


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
    snprintf(line, sizeof(line), "%s%s%s",
             it->author[0] ? it->author : "?",
             it->circle[0] ? " · " : "", it->circle);
    lv_label_set_text(s_feed_author, line);
    lv_label_set_text(s_feed_caption, it->caption);
    snprintf(line, sizeof(line), "%d/%d", s_feed_idx + 1, s_feed_n);
    lv_label_set_text(s_feed_counter, line);
    if (s_feed_circle_btn) {
        lv_obj_t *lb = lv_obj_get_child(s_feed_circle_btn, 0);
        if (lb) lv_label_set_text(lb, s_feed_circle_only ? "小圈" : "全部");
    }
    lv_obj_invalidate(s_feed_canvas);
}

static void on_feed_circle_toggle(lv_event_t *e)
{
    (void)e;
    s_feed_auto = false;
    s_feed_circle_only = !s_feed_circle_only;
    s_feed_n = feed_snapshot_filtered();
    if (s_feed_idx >= s_feed_n) s_feed_idx = 0;
    if (s_feed_n == 0) {
        toast_show(s_feed_circle_only ? "小圈暂无照片" : "暂无好友照片");
        lv_obj_add_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    render_feed();
}

static void open_feed(void)
{
    s_feed_auto = false;
    close_panel();
    s_feed_n = feed_snapshot_filtered();
    pvc_net_signal_feed();                 /* 顺手触发一次刷新 */
    if (s_feed_n == 0) {
        toast_show(s_feed_circle_only ? "小圈暂无照片" : "暂无好友照片");
        return;
    }
    if (s_feed_idx >= s_feed_n) s_feed_idx = 0;
    lv_obj_set_style_opa(s_feed_panel, LV_OPA_COVER, 0);
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

/* 轻回应 (规范 §3.3-B): 单击照片任意位置 -> 按压处散星 + 发 like;
 * 连击不限次 (每 tap 一颗星一次 like, 反应走异步队列, 云端限额兜底) */
static void on_feed_canvas_click(lv_event_t *e)
{
    (void)e;
    if (s_sleeping) { sleep_wake(); return; }   /* 息屏: 单击唤醒 (§7) */
    s_feed_auto = false;
    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        spawn_star_at(p.x, p.y);
    }
    PVC_EV("like via=tap");
    do_like();
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

/* 到达仪式收起: 20s TTL 到点后 500ms 淡出 (规范 §3.3-A/§4);
 * 期间任何交互 (s_feed_auto 已清) 则保留页面 */
static void arrival_fade_opa(void *o, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
}
static void arrival_fade_done(lv_anim_t *a)
{
    (void)a;
    if (s_feed_panel) {
        lv_obj_add_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_feed_panel, LV_OPA_COVER, 0);
    }
    s_feed_auto = false;
}
static void arrival_hide_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    s_arrival_timer = NULL;
    if (s_feed_auto && s_feed_panel) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_feed_panel);
        lv_anim_set_exec_cb(&a, arrival_fade_opa);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_duration(&a, 500);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_ready_cb(&a, arrival_fade_done);
        lv_anim_start(&a);
    } else {
        s_feed_auto = false;
    }
}

/* pvc_net 回调 (联网任务): feed 有更新 / 本人照片被赞 */
void ui_net_feed_updated(int total, int fresh, int new_likes)
{
    if (!bsp_display_lock(1000)) return;

    /* 息屏模式 (§7): 静默入库——不亮屏、不闪现、不发声, 唤醒时补播 */
    if (s_sleeping) {
        s_sleep_missed += fresh;
        bsp_display_unlock();
        return;
    }

    if (new_likes > 0) {                  /* 被赞: 飘字 + 音效 */
        pvc_sound_play(PVC_SND_LIKE);
        spawn_like_float(new_likes);
    }

    if (fresh > 0) {
        /* 到达仪式: 亮屏 + 提示音 + 自动展示最新一张 3s (docs T17) */
        pvc_sound_play(PVC_SND_DING);
        bsp_display_backlight_on();
        lv_display_trigger_activity(NULL);   /* 重置省电空闲计时 */
        s_feed_n = feed_snapshot_filtered();
        PVC_EV("arrival fresh=%d shown=%d", fresh, (int)(s_feed_n > 0));
        if (s_feed_n > 0 && s_feed_panel && s_feed_buf) {
            s_feed_idx = 0;
            s_feed_auto = true;
            lv_obj_set_style_opa(s_feed_panel, LV_OPA_COVER, 0);
            lv_obj_clear_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_feed_panel);
            render_feed();
            if (s_arrival_timer) lv_timer_del(s_arrival_timer);
            /* 小圈闪现停留 20s, 到点自动淡出归入历史 (规范 TTL 机制) */
            s_arrival_timer = lv_timer_create(arrival_hide_cb, 20000, NULL);
        }
        char msg[32];
        snprintf(msg, sizeof(msg), "新照片 +%d", fresh);
        toast_show(msg);
    } else if (s_feed_panel && !lv_obj_has_flag(s_feed_panel, LV_OBJ_FLAG_HIDDEN)) {
        /* 浏览页开着时就地刷新 */
        s_feed_n = feed_snapshot_filtered();
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
/* 图标只有 24~52px，但真实热区覆盖屏幕底部三等分。 */
static void style_tool_btn(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(btn, 246, LV_STATE_PRESSED);
}

/* ---- 手绘线稿图标 ----
 * 自制 Noto Sans SC 子集字体不含 LV_SYMBOL 私用区字形 (真机实测全部
 * 渲染为空白), 图标一律用 lv_line/圆点手绘, 同时契合低分辨率极简风。
 * lv_line_set_points 不拷贝点数组, 必须使用静态存储。 */
static lv_obj_t *icon_line(lv_obj_t *parent, const lv_point_precise_t *pts,
                           uint32_t n, lv_color_t color, int32_t width)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, pts, n);
    lv_obj_set_style_line_color(l, color, 0);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *icon_dot(lv_obj_t *parent, int32_t x, int32_t y,
                          int32_t d, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* 滤镜 = 三条滑杆 + 错位的滑块点 (28x22) */
static const lv_point_precise_t P_FL1[] = {{0, 0}, {28, 0}};
static const lv_point_precise_t P_FL2[] = {{0, 11}, {28, 11}};
static const lv_point_precise_t P_FL3[] = {{0, 22}, {28, 22}};
static void icon_filter(lv_obj_t *parent, int32_t x, int32_t y, lv_color_t c)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_set_size(g, 28, 23);
    lv_obj_set_pos(g, x, y);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
    /* lv_line 自动取自身点集包围盒, 三条线分别定位 */
    lv_obj_t *l1 = icon_line(g, P_FL1, 2, c, 2);
    lv_obj_set_pos(l1, 0, 0);
    lv_obj_t *l2 = icon_line(g, P_FL2, 2, c, 2);
    lv_obj_set_pos(l2, 0, 11);
    lv_obj_t *l3 = icon_line(g, P_FL3, 2, c, 2);
    lv_obj_set_pos(l3, 0, 22);
    icon_dot(g, 16, -3, 7, c);    /* 第一根滑块偏右 */
    icon_dot(g, 5, 8, 7, c);      /* 第二根偏左 */
    icon_dot(g, 19, 19, 7, c);    /* 第三根偏右 */
}

/* 镜像 = 中轴竖线 + 左右相对箭头 (28x24) */
static const lv_point_precise_t P_MIR_AXIS[] = {{14, 0}, {14, 24}};
static const lv_point_precise_t P_MIR_L[] = {{9, 6}, {2, 12}, {9, 18}};
static const lv_point_precise_t P_MIR_R[] = {{19, 6}, {26, 12}, {19, 18}};
static void icon_mirror(lv_obj_t *parent, int32_t x, int32_t y, lv_color_t c)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_set_size(g, 28, 25);
    lv_obj_set_pos(g, x, y);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *a = icon_line(g, P_MIR_AXIS, 2, c, 2);
    lv_obj_set_pos(a, 14, 0);
    lv_obj_t *l = icon_line(g, P_MIR_L, 3, c, 2);
    lv_obj_set_pos(l, 2, 6);
    lv_obj_t *r = icon_line(g, P_MIR_R, 3, c, 2);
    lv_obj_set_pos(r, 19, 6);
    (void)a; (void)l; (void)r;
}

/* 取消 = 手绘交叉线 (22x22) */
static const lv_point_precise_t P_CLOSE_A[] = {{0, 0}, {22, 22}};
static const lv_point_precise_t P_CLOSE_B[] = {{22, 0}, {0, 22}};
static void icon_close(lv_obj_t *parent, int32_t x, int32_t y, lv_color_t c)
{
    lv_obj_t *a = icon_line(parent, P_CLOSE_A, 2, c, 3);
    lv_obj_set_pos(a, x, y);
    lv_obj_t *b = icon_line(parent, P_CLOSE_B, 2, c, 3);
    lv_obj_set_pos(b, x, y);
    (void)a; (void)b;
}

/* 发送 = 上升箭头 (22x26) */
static const lv_point_precise_t P_SEND_STEM[] = {{11, 26}, {11, 4}};
static const lv_point_precise_t P_SEND_HEAD[] = {{3, 12}, {11, 4}, {19, 12}};
static void icon_send(lv_obj_t *parent, int32_t x, int32_t y, lv_color_t c)
{
    lv_obj_t *a = icon_line(parent, P_SEND_STEM, 2, c, 3);
    lv_obj_set_pos(a, x, y);
    lv_obj_t *b = icon_line(parent, P_SEND_HEAD, 3, c, 3);
    lv_obj_set_pos(b, x, y);
    (void)a; (void)b;
}

/* 信封 = 矩形框 + 翻盖折线 (30x20), 用于上传过渡 */
static const lv_point_precise_t P_ENV_FLAP[] = {{0, 0}, {15, 11}, {30, 0}};
static void icon_envelope(lv_obj_t *parent, int32_t x, int32_t y, lv_color_t c)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 30, 20);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, c, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *flap = icon_line(box, P_ENV_FLAP, 3, c, 2);
    lv_obj_set_pos(flap, 0, 0);
    (void)flap;
}

static void btn_shutter_cb(lv_event_t *e) { (void)e; take_photo(); }
static void btn_album_cb(lv_event_t *e)   { (void)e; open_album(); }
static void btn_filter_cb(lv_event_t *e)  { (void)e; open_filter_panel(); }
static void btn_beauty_cb(lv_event_t *e)  { (void)e; open_beauty_panel(); }
static void btn_mirror_cb(lv_event_t *e)
{
    (void)e;
    s_mirror = !s_mirror;
    s_thumb_dirty = true;
    if (s_mirror_label) {
        lv_obj_set_style_bg_color(s_mirror_label,
            s_mirror ? lv_color_make(0xf8, 0xf5, 0xee)
                     : lv_color_make(0xbd, 0x4a, 0x3a), 0);
    }
}
static void btn_feed_cb(lv_event_t *e)
{
    (void)e;
    set_mode(MODE_CIRCLE);
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
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    hw2d_init();
    update_active_filter();

    /* ---------- 全屏预览 canvas ---------- */
    s_canvas_buf = PSRAM_MALLOC(FRAME_BYTES);
    if (!s_canvas_buf) { ESP_LOGE(TAG, "canvas PSRAM alloc failed"); return; }
    s_review_raw_buf = PSRAM_MALLOC(FRAME_BYTES);
    s_review_fx_buf = PSRAM_MALLOC(FRAME_BYTES);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, UI_W, UI_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_style_bg_opa(s_canvas, LV_OPA_TRANSP, 0);
    /* 相机模式手势: 左右轻划切滤镜 / 上滑退出 (规范 §5)。
     * 关自身滚动, 手势由手动 touch_diag 累计位移后派发。 */
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    layer_gesture_register(s_canvas);

    /* 顶部只保留关系入口与联网圆点，不占一整条状态栏。 */
    lv_obj_t *feed_hot = lv_btn_create(scr);
    s_feed_hot = feed_hot;
    lv_obj_set_size(feed_hot, 72, 58);
    lv_obj_set_pos(feed_hot, 0, 0);
    style_tool_btn(feed_hot);
    lv_obj_add_flag(feed_hot, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(feed_hot, btn_feed_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(feed_hot, btn_flip_long_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *orbit = lv_arc_create(feed_hot);
    lv_obj_set_size(orbit, 30, 30);
    lv_obj_center(orbit);
    lv_arc_set_bg_angles(orbit, 0, 360);
    lv_arc_set_angles(orbit, 0, 360);
    lv_obj_set_style_arc_color(orbit, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(orbit, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(orbit, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(orbit, 2, LV_PART_INDICATOR);
    lv_obj_remove_style(orbit, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(orbit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *orbit_dot = lv_obj_create(feed_hot);
    lv_obj_set_size(orbit_dot, 7, 7);
    lv_obj_set_pos(orbit_dot, 42, 13);
    lv_obj_set_style_bg_color(orbit_dot, lv_color_make(0xbd, 0x4a, 0x3a), 0);
    lv_obj_set_style_border_width(orbit_dot, 0, 0);
    lv_obj_set_style_radius(orbit_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(orbit_dot, 0, 0);
    lv_obj_remove_flag(orbit_dot, LV_OBJ_FLAG_CLICKABLE);

    s_status_l = NULL;
    s_status_c = NULL;
    s_status_r = lv_obj_create(scr);
    lv_obj_set_size(s_status_r, 7, 7);
    lv_obj_set_pos(s_status_r, UI_W - 19, 14);
    lv_obj_set_style_bg_color(s_status_r, lv_color_make(0xbd, 0x4a, 0x3a), 0);
    lv_obj_set_style_border_width(s_status_r, 0, 0);
    lv_obj_set_style_radius(s_status_r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_status_r, 0, 0);
    lv_obj_remove_flag(s_status_r, LV_OBJ_FLAG_CLICKABLE);

    /* 滤镜名 tag (规范画面 2): 顶部居中, 切换滤镜时短暂出现 */
    s_filter_tag = lv_label_create(scr);
    lv_obj_set_style_bg_color(s_filter_tag, COL_NIGHT, 0);
    lv_obj_set_style_bg_opa(s_filter_tag, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_filter_tag, 10, 0);
    lv_obj_set_style_pad_hor(s_filter_tag, 10, 0);
    lv_obj_set_style_pad_ver(s_filter_tag, 2, 0);
    lv_obj_set_style_text_color(s_filter_tag, COL_PAPER, 0);
    lv_obj_align(s_filter_tag, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_add_flag(s_filter_tag, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_filter_tag, LV_OBJ_FLAG_CLICKABLE);

    /* 滤镜缩略图只在弹出面板显示; 不常驻遮挡低分辨率取景画面。 */
    s_thumb_raw = PSRAM_MALLOC(THUMB_SZ * THUMB_SZ * 2);
    for (int i = 0; i < HW2D_FILTER_MAX; i++) {
        s_thumb_bufs[i] = PSRAM_MALLOC(THUMB_SZ * THUMB_SZ * 2);
    }

    /* ---------- 底部三分区：小图标，大热区 ---------- */
    lv_obj_t *bar = lv_obj_create(scr);
    s_bar = bar;
    lv_obj_set_size(bar, UI_W, BAR_H);
    lv_obj_set_pos(bar, 0, UI_H - BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_shadow_width(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bar, on_layer_long_press, LV_EVENT_LONG_PRESSED, NULL);
    layer_gesture_register(bar);

    /* 滤镜：点击滤镜，长按美颜；整块 104x60 都可点。 */
    lv_obj_t *b_filter = lv_btn_create(bar);
    lv_obj_set_size(b_filter, 104, BAR_H);
    lv_obj_set_pos(b_filter, 0, 0);
    style_tool_btn(b_filter);
    lv_obj_add_flag(b_filter, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(b_filter, btn_filter_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b_filter, btn_beauty_cb, LV_EVENT_LONG_PRESSED, NULL);
    icon_filter(b_filter, (104 - 28) / 2, (BAR_H - 23) / 2,
                lv_color_make(0xf8, 0xf5, 0xee));

    /* 快门 Ø44 白圈 3px 描边 (规范 §5); 112x60 热区, 按下立即缩放反馈。 */
    lv_obj_t *b_shutter = lv_btn_create(bar);
    lv_obj_set_size(b_shutter, 112, BAR_H);
    lv_obj_set_pos(b_shutter, 104, 0);
    style_tool_btn(b_shutter);
    lv_obj_add_flag(b_shutter, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(b_shutter, btn_shutter_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b_shutter, btn_album_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *shutter_ring = lv_obj_create(b_shutter);
    lv_obj_set_size(shutter_ring, 44, 44);
    lv_obj_center(shutter_ring);
    lv_obj_set_style_bg_opa(shutter_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(shutter_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(shutter_ring, 3, 0);
    lv_obj_set_style_border_color(shutter_ring, COL_PAPER, 0);
    lv_obj_set_style_pad_all(shutter_ring, 0, 0);
    lv_obj_remove_flag(shutter_ring, LV_OBJ_FLAG_CLICKABLE);

    /* 自拍方向：104x60 热区。 */
    lv_obj_t *b_mirror = lv_btn_create(bar);
    lv_obj_set_size(b_mirror, 104, BAR_H);
    lv_obj_set_pos(b_mirror, 216, 0);
    style_tool_btn(b_mirror);
    lv_obj_add_flag(b_mirror, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(b_mirror, btn_mirror_cb, LV_EVENT_CLICKED, NULL);
    s_mirror_label = lv_obj_create(b_mirror);   /* 复用为开关指示点 */
    lv_obj_set_size(s_mirror_label, 7, 7);
    lv_obj_set_pos(s_mirror_label, 104 / 2 + 22, 8);
    lv_obj_set_style_bg_color(s_mirror_label, lv_color_make(0xf8, 0xf5, 0xee), 0);
    lv_obj_set_style_bg_opa(s_mirror_label, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mirror_label, 0, 0);
    lv_obj_set_style_radius(s_mirror_label, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_mirror_label, 0, 0);
    lv_obj_remove_flag(s_mirror_label, LV_OBJ_FLAG_CLICKABLE);
    icon_mirror(b_mirror, (104 - 28) / 2, (BAR_H - 25) / 2,
                lv_color_make(0xf8, 0xf5, 0xee));

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
    /* 小圈/大圈流手势: 左右翻页 / 下滑拍照 / 上滑系统页; 长按=选择盘。
     * 必须关滚动, 否则手势被滚动逻辑吃掉 */
    lv_obj_remove_flag(s_feed_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_feed_panel, on_layer_long_press,
                        LV_EVENT_LONG_PRESSED, NULL);
    layer_gesture_register(s_feed_panel);

    if (s_feed_buf) {
        s_feed_canvas = lv_canvas_create(s_feed_panel);
        lv_canvas_set_buffer(s_feed_canvas, s_feed_buf, UI_W, UI_H,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_feed_canvas, 0, 0);
        lv_obj_set_style_bg_opa(s_feed_canvas, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s_feed_canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_feed_canvas, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(s_feed_canvas, on_feed_canvas_click,
                            LV_EVENT_CLICKED, NULL);
    }

    /* 昵称贴纸 (规范画面 1): 左上 8,8 丁香紫 85% 底 + 1px 白边 + R4 + -2° */
    s_feed_author = lv_label_create(s_feed_panel);
    lv_obj_set_pos(s_feed_author, 8, 8);
    lv_obj_set_style_bg_color(s_feed_author, COL_LILAC, 0);
    lv_obj_set_style_bg_opa(s_feed_author, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_feed_author, 1, 0);
    lv_obj_set_style_border_color(s_feed_author, lv_color_white(), 0);
    lv_obj_set_style_radius(s_feed_author, 4, 0);
    lv_obj_set_style_pad_hor(s_feed_author, 8, 0);
    lv_obj_set_style_pad_ver(s_feed_author, 2, 0);
    lv_obj_set_style_text_color(s_feed_author, COL_NIGHT, 0);
    lv_obj_set_style_shadow_width(s_feed_author, 2, 0);
    lv_obj_set_style_shadow_color(s_feed_author, COL_NIGHT, 0);
    lv_obj_set_style_shadow_opa(s_feed_author, LV_OPA_20, 0);
    lv_obj_set_style_shadow_offset_x(s_feed_author, 2, 0);
    lv_obj_set_style_shadow_offset_y(s_feed_author, 2, 0);
    lv_obj_set_style_transform_rotation(s_feed_author, -20, 0);  /* -2.0° */

    /* 底部信息条: 配文 + 计数 */
    lv_obj_t *fbar = lv_obj_create(s_feed_panel);
    lv_obj_set_size(fbar, UI_W, 36);
    lv_obj_set_pos(fbar, 0, UI_H - 36);
    lv_obj_set_style_bg_color(fbar, lv_color_make(0x00, 0x00, 0x00), 0);
    lv_obj_set_style_bg_opa(fbar, LV_OPA_60, 0);
    lv_obj_set_style_border_width(fbar, 0, 0);
    lv_obj_set_style_pad_all(fbar, 0, 0);
    lv_obj_set_style_radius(fbar, 0, 0);
    lv_obj_set_style_shadow_width(fbar, 0, 0);

    s_feed_caption = lv_label_create(fbar);
    lv_obj_set_pos(s_feed_caption, 8, 10);
    lv_obj_set_width(s_feed_caption, UI_W - 120);
    lv_label_set_long_mode(s_feed_caption, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_feed_caption, lv_color_make(0xc0, 0xc6, 0xd2), 0);
    s_feed_counter = lv_label_create(fbar);
    lv_obj_set_pos(s_feed_counter, UI_W - 44, 2);
    lv_obj_set_style_text_color(s_feed_counter, lv_color_white(), 0);

    /* 返回 / 上一张 / 下一张 / 点赞 */
    lv_obj_t *f_back = lv_btn_create(s_feed_panel);
    lv_obj_add_flag(f_back, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(f_back, 52, 28);
    lv_obj_set_pos(f_back, 8, 6);
    lv_obj_add_event_cb(f_back, on_feed_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *f_back_l = lv_label_create(f_back);
    lv_label_set_text(f_back_l, "< 返回");
    lv_obj_center(f_back_l);

    lv_obj_t *f_prev = lv_btn_create(s_feed_panel);
    lv_obj_add_flag(f_prev, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(f_prev, 36, 48);
    lv_obj_set_pos(f_prev, 4, (UI_H - 48) / 2);
    lv_obj_set_style_bg_opa(f_prev, LV_OPA_40, 0);
    lv_obj_add_event_cb(f_prev, on_feed_prev, LV_EVENT_CLICKED, NULL);
    lv_obj_t *f_prev_l = lv_label_create(f_prev);
    lv_label_set_text(f_prev_l, "<");   /* LV_SYMBOL 在 CJK 字体下缺字 */
    lv_obj_center(f_prev_l);

    lv_obj_t *f_next = lv_btn_create(s_feed_panel);
    lv_obj_add_flag(f_next, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(f_next, 36, 48);
    lv_obj_set_pos(f_next, UI_W - 40, (UI_H - 48) / 2);
    lv_obj_set_style_bg_opa(f_next, LV_OPA_40, 0);
    lv_obj_add_event_cb(f_next, on_feed_next, LV_EVENT_CLICKED, NULL);
    lv_obj_t *f_next_l = lv_label_create(f_next);
    lv_label_set_text(f_next_l, ">");
    lv_obj_center(f_next_l);

    lv_obj_t *f_heart = lv_btn_create(s_feed_panel);
    lv_obj_add_flag(f_heart, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(f_heart, 64, 30);          /* "点赞" 2x16px + 内边距 */
    lv_obj_set_pos(f_heart, UI_W - 72, 6);
    lv_obj_set_style_bg_color(f_heart, lv_color_make(0xe8, 0x4a, 0x4a), 0);
    lv_obj_add_event_cb(f_heart, on_feed_heart, LV_EVENT_CLICKED, NULL);
    lv_obj_t *f_heart_l = lv_label_create(f_heart);
    lv_label_set_text(f_heart_l, "点赞");
    lv_obj_set_style_text_color(f_heart_l, lv_color_white(), 0);
    lv_obj_center(f_heart_l);

    /* 全部/小圈 切换 (小圈互动入口: 只看小圈内容, 双击或按钮点赞) */
    s_feed_circle_btn = lv_btn_create(s_feed_panel);
    lv_obj_add_flag(s_feed_circle_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(s_feed_circle_btn, 64, 30);
    lv_obj_set_pos(s_feed_circle_btn, UI_W - 144, 6);
    lv_obj_set_style_bg_color(s_feed_circle_btn, lv_color_make(0x28, 0x30, 0x40), 0);
    lv_obj_add_event_cb(s_feed_circle_btn, on_feed_circle_toggle,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *f_circle_l = lv_label_create(s_feed_circle_btn);
    lv_label_set_text(f_circle_l, "全部");
    lv_obj_set_style_text_color(f_circle_l, lv_color_white(), 0);
    lv_obj_center(f_circle_l);

    /* ---------- 常驻模式画布 (v1.1 §1: 默认屏 = 常驻主图/足迹) ---------- */
    s_home_buf = PSRAM_MALLOC(FRAME_BYTES);
    if (s_home_buf) {
        s_home_canvas = lv_canvas_create(scr);
        lv_canvas_set_buffer(s_home_canvas, s_home_buf, UI_W, UI_H,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_home_canvas, 0, 0);
        lv_obj_set_style_bg_opa(s_home_canvas, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(s_home_canvas, LV_OBJ_FLAG_SCROLLABLE);
        hw2d_fill(s_home_buf, UI_W * UI_H, rgb565(0x1a, 0x1d, 0x1a));
        /* HOME 手势: 左右翻足迹 / 下滑拍照 / 上滑系统页 / 长按选择盘
         * 必须关滚动, 否则手势被滚动逻辑吃掉 */
        lv_obj_remove_flag(s_home_canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_home_canvas, on_layer_long_press,
                            LV_EVENT_LONG_PRESSED, NULL);
        layer_gesture_register(s_home_canvas);
        lv_obj_add_flag(s_home_canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_home_canvas, on_home_click,
                            LV_EVENT_CLICKED, NULL);

        /* 空态提示 (首次使用, 无足迹无云端主图) */
        s_home_hint = lv_label_create(scr);
        lv_label_set_text(s_home_hint, "下滑拍照");
        lv_obj_set_style_text_color(s_home_hint, COL_PAPER, 0);
        lv_obj_set_style_text_opa(s_home_hint, LV_OPA_60, 0);
        lv_obj_center(s_home_hint);
    }

    /* 模式名贴纸 (切模式浮现 1.5s) */
    s_mode_tag = lv_label_create(scr);
    lv_obj_set_style_bg_color(s_mode_tag, COL_LILAC, 0);
    lv_obj_set_style_bg_opa(s_mode_tag, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_mode_tag, 1, 0);
    lv_obj_set_style_border_color(s_mode_tag, lv_color_white(), 0);
    lv_obj_set_style_radius(s_mode_tag, 4, 0);
    lv_obj_set_style_pad_hor(s_mode_tag, 8, 0);
    lv_obj_set_style_pad_ver(s_mode_tag, 2, 0);
    lv_obj_set_style_text_color(s_mode_tag, COL_NIGHT, 0);
    lv_obj_align(s_mode_tag, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_add_flag(s_mode_tag, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_mode_tag, LV_OBJ_FLAG_CLICKABLE);

    /* 轻系统页 (上滑呼出) */
    sheet_build(scr);

    update_status();

    /* 初始模式: 有足迹/云端内容则常驻屏, 否则直接相机 (首次使用引导) */
    scan_footprints();
    set_mode(s_foot_n > 0 ? MODE_HOME : MODE_CAMERA);

    /* 预览刷新定时器: 12.5 FPS。自拍取景足够流畅, 且把 core0/LVGL 任务
     * 从全屏渲染中解放出来 —— 25FPS 时触摸采样被饿死, 点按明显迟钝 */
    lv_timer_create(preview_timer_cb, 80, NULL);

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
    if (!s_publish_choice_q)
        s_publish_choice_q = xQueueCreate(2, sizeof(publish_choice_t));
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

void ui_net_set_status(const char *txt)
{
    if (!bsp_display_lock(1000)) return;
    if (s_status_r) {
        bool online = txt && strcmp(txt, "Online") == 0;
        lv_obj_set_style_bg_color(s_status_r,
            online ? lv_color_make(0xf8, 0xf5, 0xee)
                   : lv_color_make(0xbd, 0x4a, 0x3a), 0);
        lv_obj_set_style_opa(s_status_r, online ? LV_OPA_70 : LV_OPA_COVER, 0);
    }
    bsp_display_unlock();
}
