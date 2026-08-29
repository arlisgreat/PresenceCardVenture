/*
 * hw2d.c - CoreS3-Lite (ESP32-S3) 硬件 2D 加速层实现
 *
 * 设计要点:
 *   - 全部像素算子是整数定点, 无浮点、无除法热点 (磨皮均值用查表)。
 *   - 逐 4/8 像素批次处理, 顺序访问, 对 PSRAM 缓存友好。
 *   - IDF 环境用 PSRAM 做中间平面 / 性能用 esp_timer; 非 IDF 环境
 *     (如 macOS 语法自检) 自动降级为标准 C。
 */
#include "hw2d.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#define HW2D_MALLOC(sz)  heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define HW2D_FREE(p)     heap_caps_free(p)
#define HW2D_TIME_US()   ((uint32_t)esp_timer_get_time())
#else
#include <time.h>
#define HW2D_MALLOC(sz)  malloc(sz)
#define HW2D_FREE(p)     free(p)
#define HW2D_TIME_US()   ((uint32_t)((clock() * 1000000) / CLOCKS_PER_SEC))
#endif

/* ================================================================== */
/* 内置滤镜参数                                                        */
/* ================================================================== */
static const hw2d_filter_t s_filter_tbl[HW2D_FILTER_MAX] = {
    /*            bias contrast sat gain_r gain_g gain_b */
    /* ORIGINAL */ {   0,     100, 100,   100,   100,   100 },
    /* FAIR    */ {  18,     106, 104,   100,   106,   116 },
    /* WARM    */ {   8,     110, 112,   116,   104,    88 },
    /* COOL    */ {  -2,     104, 106,    90,   106,   118 },
    /* BW      */ {   0,     120,   0,   100,   100,   100 },
    /* VINTAGE */ {  -6,     114,  92,   118,   102,    82 },
};

const hw2d_filter_t *hw2d_filter_get(hw2d_filter_id_t id)
{
    if (id >= HW2D_FILTER_MAX) {
        id = HW2D_FILTER_ORIGINAL;
    }
    return &s_filter_tbl[id];
}

/* ================================================================== */
/* 工具                                                                */
/* ================================================================== */
static inline uint8_t clamp_u8(int v)
{
    return (uint8_t)((v < 0) ? 0 : (v > 255) ? 255 : v);
}

/* 由滤镜参数生成对比度/亮度/饱和/增益合成后的 8bit 通道 LUT (灰阶近似) */
static void build_channel_lut(const hw2d_filter_t *f, uint16_t gain_q8,
                              uint8_t *lut)
{
    int kc = (f->contrast * 128) / 100;   /* Q7, 100% -> 128 */
    int ks = (f->sat * 128) / 100;        /* Q7 */
    int i;

    for (i = 0; i < 256; i++) {
        int gray = i;                     /* 灰阶近似: 假设 r=g=b */
        int v;
        v = ((gray - 128) * kc) / 128 + 128; /* 对比度 */
        v = clamp_u8(v + f->bias);           /* 亮度 (美白), 与 exact 路径同步钳位 */
        v = clamp_u8(gray + ((v - gray) * ks) / 128);  /* 饱和 */
        v = (v * (int)gain_q8) / 256;        /* 通道增益 */
        lut[i] = clamp_u8(v);
    }
}

/* 灰度专用 LUT (sat==0 且三通道增益相同时, 先算真实亮度 Y 再查表) */
static void build_gray_lut(const hw2d_filter_t *f, uint8_t *lut)
{
    int kc = (f->contrast * 128) / 100;
    int kg = (f->gain_r * 256) / 100;
    int i;

    for (i = 0; i < 256; i++) {
        int v = clamp_u8(((i - 128) * kc) / 128 + 128 + f->bias);
        v = (v * kg) / 256;
        lut[i] = clamp_u8(v);
    }
}

/* 解包 RGB565 -> 8bit 通道 */
#define UNPACK_R(p) (uint8_t)((((((p) >> 11) & 0x1F)) << 3) | (((p) >> 13) & 0x07))
#define UNPACK_G(p) (uint8_t)((((((p) >> 5) & 0x3F)) << 2) | (((p) >> 9) & 0x03))
#define UNPACK_B(p) (uint8_t)((((((p) & 0x1F)) << 3) | (((p) >> 2) & 0x07)))
#define PACK_RGB565(r, g, b) \
    (uint16_t)(((((r) & 0xF8)) << 8) | ((((g) & 0xFC)) << 3) | (((b)) >> 3))

/* ================================================================== */
/* 滤镜 LUT 快路径 (预览)                                              */
/* ================================================================== */
static uint8_t s_lut_r[256];
static uint8_t s_lut_g[256];
static uint8_t s_lut_b[256];
static uint8_t s_lut_gray[256];
static hw2d_filter_t s_lut_cached;   /* 上次建表时的参数快照 */
static bool s_lut_valid = false;
static bool s_lut_is_gray = false;

/* 是否可用灰度专用路径: 饱和 0 且三通道增益一致 */
static inline bool filter_is_gray(const hw2d_filter_t *f)
{
    return f->sat == 0 && f->gain_r == f->gain_g && f->gain_g == f->gain_b;
}

/* 按内容判断并重建 LUT (UI 复用同一结构体地址改参数, 指针比较会漏重建) */
static void ensure_lut(const hw2d_filter_t *f)
{
    if (s_lut_valid && memcmp(&s_lut_cached, f, sizeof(*f)) == 0) return;
    s_lut_is_gray = filter_is_gray(f);
    if (s_lut_is_gray) {
        build_gray_lut(f, s_lut_gray);
    } else {
        build_channel_lut(f, f->gain_r * 256 / 100, s_lut_r);
        build_channel_lut(f, f->gain_g * 256 / 100, s_lut_g);
        build_channel_lut(f, f->gain_b * 256 / 100, s_lut_b);
    }
    s_lut_cached = *f;
    s_lut_valid = true;
}

void hw2d_apply_filter_lut(uint16_t *dst, const uint16_t *src, uint32_t npix,
                           const hw2d_filter_t *f)
{
    uint32_t i;

    ensure_lut(f);

    if (s_lut_is_gray) {
        /* 灰度路径: 逐像素真实亮度 Y -> 单通道 LUT */
        for (i = 0; i < npix; i++) {
            uint16_t p = src[i];
            uint8_t  r = UNPACK_R(p);
            uint8_t  g = UNPACK_G(p);
            uint8_t  b = UNPACK_B(p);
            uint8_t  y = (uint8_t)((r * 38 + g * 75 + b * 15) >> 7);
            dst[i] = PACK_RGB565(s_lut_gray[y], s_lut_gray[y], s_lut_gray[y]);
        }
        return;
    }

    /* 逐 4 像素批次 (uint64 读取, 缓存友好) */
    uint32_t nq = npix / 4;
    const uint64_t *sq = (const uint64_t *)src;
    uint64_t *dq = (uint64_t *)dst;
    for (i = 0; i < nq; i++) {
        uint64_t q = sq[i];
        uint16_t p0 = (uint16_t)q;
        uint16_t p1 = (uint16_t)(q >> 16);
        uint16_t p2 = (uint16_t)(q >> 32);
        uint16_t p3 = (uint16_t)(q >> 48);
        uint8_t  r, g, b;
        uint64_t o;

        r = s_lut_r[UNPACK_R(p0)]; g = s_lut_g[UNPACK_G(p0)]; b = s_lut_b[UNPACK_B(p0)];
        uint16_t o0 = PACK_RGB565(r, g, b);
        r = s_lut_r[UNPACK_R(p1)]; g = s_lut_g[UNPACK_G(p1)]; b = s_lut_b[UNPACK_B(p1)];
        uint16_t o1 = PACK_RGB565(r, g, b);
        r = s_lut_r[UNPACK_R(p2)]; g = s_lut_g[UNPACK_G(p2)]; b = s_lut_b[UNPACK_B(p2)];
        uint16_t o2 = PACK_RGB565(r, g, b);
        r = s_lut_r[UNPACK_R(p3)]; g = s_lut_g[UNPACK_G(p3)]; b = s_lut_b[UNPACK_B(p3)];
        uint16_t o3 = PACK_RGB565(r, g, b);

        o = (uint64_t)o0 | ((uint64_t)o1 << 16) | ((uint64_t)o2 << 32) | ((uint64_t)o3 << 48);
        dq[i] = o;
    }
    for (i = nq * 4; i < npix; i++) {
        uint16_t p = src[i];
        uint8_t  r = s_lut_r[UNPACK_R(p)];
        uint8_t  g = s_lut_g[UNPACK_G(p)];
        uint8_t  b = s_lut_b[UNPACK_B(p)];
        dst[i] = PACK_RGB565(r, g, b);
    }
}

/*
 * 预览专用融合算子: 2:1 盒均值降采样 + LUT 滤镜, 单遍完成。
 * src 尺寸必须恰为 (2*dw) x (2*dh) (VGA->QVGA)。较 "双线性缩放 + LUT"
 * 两遍省一次 QVGA 缓冲读写, 且 2x2 均值只做加法移位。
 * 与预览 LUT 共享静态表 —— 仅限单任务 (LVGL/core0) 使用。
 */
void hw2d_scale2x_lut(uint16_t *dst, const uint16_t *src,
                      uint32_t dw, uint32_t dh, const hw2d_filter_t *f)
{
    ensure_lut(f);
    uint32_t sw = dw * 2;
    for (uint32_t y = 0; y < dh; y++) {
        const uint16_t *r0 = src + (size_t)(y * 2) * sw;
        const uint16_t *r1 = r0 + sw;
        uint16_t *o = dst + (size_t)y * dw;
        for (uint32_t x = 0; x < dw; x++) {
            uint16_t p00 = r0[x * 2], p01 = r0[x * 2 + 1];
            uint16_t p10 = r1[x * 2], p11 = r1[x * 2 + 1];
            uint8_t r = (uint8_t)((UNPACK_R(p00) + UNPACK_R(p01) +
                                   UNPACK_R(p10) + UNPACK_R(p11)) >> 2);
            uint8_t g = (uint8_t)((UNPACK_G(p00) + UNPACK_G(p01) +
                                   UNPACK_G(p10) + UNPACK_G(p11)) >> 2);
            uint8_t b = (uint8_t)((UNPACK_B(p00) + UNPACK_B(p01) +
                                   UNPACK_B(p10) + UNPACK_B(p11)) >> 2);
            if (s_lut_is_gray) {
                uint8_t yv = (uint8_t)((r * 38 + g * 75 + b * 15) >> 7);
                uint8_t v = s_lut_gray[yv];
                o[x] = PACK_RGB565(v, v, v);
            } else {
                o[x] = PACK_RGB565(s_lut_r[r], s_lut_g[g], s_lut_b[b]);
            }
        }
    }
}

/* ================================================================== */
/* 精确滤镜 (拍照保存, 每像素独立饱和)                                 */
/* ================================================================== */
void hw2d_apply_filter_exact(uint16_t *dst, const uint16_t *src, uint32_t npix,
                             const hw2d_filter_t *f)
{
    int kc = (f->contrast * 128) / 100;
    int ks = (f->sat * 128) / 100;
    int kgr = (f->gain_r * 256) / 100;
    int kgg = (f->gain_g * 256) / 100;
    int kgb = (f->gain_b * 256) / 100;
    uint32_t i;

    for (i = 0; i < npix; i++) {
        uint16_t p = src[i];
        int r = UNPACK_R(p);
        int g = UNPACK_G(p);
        int b = UNPACK_B(p);
        int gray = (r * 38 + g * 75 + b * 15) >> 7; /* 亮度 Y, 权重和 128 */
        int nr, ng, nb;

        /* 对比度 */
        nr = ((r - 128) * kc) / 128 + 128;
        ng = ((g - 128) * kc) / 128 + 128;
        nb = ((b - 128) * kc) / 128 + 128;
        /* 亮度 */
        nr = clamp_u8(nr + f->bias);
        ng = clamp_u8(ng + f->bias);
        nb = clamp_u8(nb + f->bias);
        /* 饱和 (对真实灰度) */
        nr = clamp_u8(gray + ((nr - gray) * ks) / 128);
        ng = clamp_u8(gray + ((ng - gray) * ks) / 128);
        nb = clamp_u8(gray + ((nb - gray) * ks) / 128);
        /* 通道增益 */
        nr = clamp_u8((nr * kgr) / 256);
        ng = clamp_u8((ng * kgg) / 256);
        nb = clamp_u8((nb * kgb) / 256);

        dst[i] = PACK_RGB565(nr, ng, nb);
    }
}

/* ================================================================== */
/* 磨皮 3x3 (平面法 + 查表除法)                                        */
/* ================================================================== */
static uint8_t *s_plane_r = NULL;
static uint8_t *s_plane_g = NULL;
static uint8_t *s_plane_b = NULL;
static uint8_t  s_avg_lut[2296]; /* 均值查表: 0..2295 (8bit 通道 9x255) */
static uint32_t s_blur_w = 0;
static uint32_t s_blur_h = 0;

esp_err_t hw2d_blur_prepare(uint32_t w, uint32_t h)
{
    uint32_t bytes = w * h;
    int i;

    if (s_plane_r && s_blur_w == w && s_blur_h == h) {
        return ESP_OK;
    }
    hw2d_blur_deinit();

    s_plane_r = HW2D_MALLOC(bytes);
    s_plane_g = HW2D_MALLOC(bytes);
    s_plane_b = HW2D_MALLOC(bytes);
    if (!s_plane_r || !s_plane_g || !s_plane_b) {
        hw2d_blur_deinit();
        return ESP_ERR_NO_MEM;
    }
    for (i = 0; i < 2296; i++) {
        s_avg_lut[i] = (uint8_t)((i * 228 + 1024) >> 11); /* ~i/9, Q11 定点 */
    }
    s_blur_w = w;
    s_blur_h = h;
    return ESP_OK;
}

void hw2d_blur_deinit(void)
{
    if (s_plane_r) { HW2D_FREE(s_plane_r); s_plane_r = NULL; }
    if (s_plane_g) { HW2D_FREE(s_plane_g); s_plane_g = NULL; }
    if (s_plane_b) { HW2D_FREE(s_plane_b); s_plane_b = NULL; }
    s_blur_w = s_blur_h = 0;
}

void hw2d_blur3x3(uint16_t *out, const uint16_t *in, uint32_t w, uint32_t h,
                  uint8_t strength)
{
    uint32_t x, y;
    uint32_t bytes = w * h;

    if (strength == 0) {
        memcpy(out, in, bytes * 2);
        return;
    }
    if (hw2d_blur_prepare(w, h) != ESP_OK) {
        memcpy(out, in, bytes * 2);
        return;
    }

    /* 拆平面 (一次遍历) */
    for (y = 0; y < h; y++) {
        const uint16_t *row = in + (size_t)y * w;
        uint8_t *pr = s_plane_r + (size_t)y * w;
        uint8_t *pg = s_plane_g + (size_t)y * w;
        uint8_t *pb = s_plane_b + (size_t)y * w;
        for (x = 0; x < w; x++) {
            uint16_t p = row[x];
            pr[x] = UNPACK_R(p);
            pg[x] = UNPACK_G(p);
            pb[x] = UNPACK_B(p);
        }
    }

    /* 3x3 滑窗均值, 边界复制 */
    for (y = 0; y < h; y++) {
        uint32_t yt = (y == 0) ? 0 : y - 1;
        uint32_t yb = (y == h - 1) ? y : y + 1;
        uint16_t *orow = out + (size_t)y * w;
        const uint8_t *rt = s_plane_r + (size_t)yt * w, *rm = s_plane_r + (size_t)y * w, *rb = s_plane_r + (size_t)yb * w;
        const uint8_t *gt = s_plane_g + (size_t)yt * w, *gm = s_plane_g + (size_t)y * w, *gb = s_plane_g + (size_t)yb * w;
        const uint8_t *bt = s_plane_b + (size_t)yt * w, *bm = s_plane_b + (size_t)y * w, *bb = s_plane_b + (size_t)yb * w;

        for (x = 0; x < w; x++) {
            uint32_t xl = (x == 0) ? 0 : x - 1;
            uint32_t xr = (x == w - 1) ? x : x + 1;
            int sr = rt[xl] + rt[x] + rt[xr] + rm[xl] + rm[x] + rm[xr] + rb[xl] + rb[x] + rb[xr];
            int sg = gt[xl] + gt[x] + gt[xr] + gm[xl] + gm[x] + gm[xr] + gb[xl] + gb[x] + gb[xr];
            int sb = bt[xl] + bt[x] + bt[xr] + bm[xl] + bm[x] + bm[xr] + bb[xl] + bb[x] + bb[xr];
            uint8_t ar = s_avg_lut[sr];
            uint8_t ag = s_avg_lut[sg];
            uint8_t ab = s_avg_lut[sb];
            uint16_t ip = in[(size_t)y * w + x];
            int or_, og, ob;

            /* strength 混合: out = avg + (in - avg) * strength / 100 */
            or_ = ar + ((UNPACK_R(ip) - ar) * strength) / 100;
            og = ag + ((UNPACK_G(ip) - ag) * strength) / 100;
            ob = ab + ((UNPACK_B(ip) - ab) * strength) / 100;
            orow[x] = PACK_RGB565(clamp_u8(or_), clamp_u8(og), clamp_u8(ob));
        }
    }
}

/* ================================================================== */
/* Alpha 混合 (贴纸 SRC_OVER, 通道域 int16 无溢出)                     */
/* ================================================================== */
void hw2d_alpha_blend(uint16_t *dst, const uint16_t *src, uint32_t npix,
                      uint8_t alpha)
{
    uint32_t inv = 255u - alpha;
    uint32_t i;

    if (alpha == 0) {
        return;
    }
    if (alpha == 255) {
        memcpy(dst, src, npix * 2);
        return;
    }
    for (i = 0; i < npix; i++) {
        uint16_t p = src[i];
        uint16_t d = dst[i];
        int r = (((p >> 11) & 0x1F) * (int)alpha + ((d >> 11) & 0x1F) * (int)inv) >> 8;
        int g = (((p >> 5) & 0x3F) * (int)alpha + ((d >> 5) & 0x3F) * (int)inv) >> 8;
        int b = ((p & 0x1F) * (int)alpha + (d & 0x1F) * (int)inv) >> 8;
        dst[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

/* ================================================================== */
/* 16bit 车道字节交换 (LE<->BE)                                        */
/* ================================================================== */
#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)
#define HW2D_HAVE_PIE 1
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
extern void hw2d_pie_swap16_blk(void *dst, const void *src, uint32_t nblk);
/* PIE 单使用者互斥: 同核两任务 (LVGL/net) 抢占期间并发用 PIE 会互踩
 * q 寄存器 (不依赖 IDF 是否保存 PIE 上下文)。try-lock 失败即走标量。 */
static SemaphoreHandle_t s_pie_mtx;
#endif
static bool s_pie_swap_ok = false;

static void swap16_c(uint16_t *dst, const uint16_t *src, uint32_t npix)
{
    uint32_t i = 0;
    if ((((uintptr_t)dst | (uintptr_t)src) & 7u) == 0) {
        const uint64_t *sq = (const uint64_t *)src;
        uint64_t *dq = (uint64_t *)dst;
        uint32_t nq = npix / 4;
        uint32_t k;
        for (k = 0; k < nq; k++) {
            uint64_t q = sq[k];
            dq[k] = ((q & 0xFF00FF00FF00FF00ULL) >> 8) |
                    ((q & 0x00FF00FF00FF00FFULL) << 8);
        }
        i = nq * 4;
    }
    for (; i < npix; i++) {
        uint16_t v = src[i];
        dst[i] = (uint16_t)((v >> 8) | (v << 8));
    }
}

void hw2d_swap16(uint16_t *dst, const uint16_t *src, uint32_t npix)
{
#if HW2D_HAVE_PIE
    if (s_pie_swap_ok && s_pie_mtx &&
        ((((uintptr_t)dst | (uintptr_t)src) & 15u) == 0) &&
        xSemaphoreTake(s_pie_mtx, 0) == pdTRUE) {
        uint32_t nblk = npix / 8;            /* 8 像素 = 128bit */
        if (nblk) hw2d_pie_swap16_blk(dst, src, nblk);
        xSemaphoreGive(s_pie_mtx);
        swap16_c(dst + nblk * 8, src + nblk * 8, npix - nblk * 8);
        return;
    }
#endif
    swap16_c(dst, src, npix);
}

bool hw2d_pie_active(void)
{
    return s_pie_swap_ok;
}

/* ================================================================== */
/* 缩放 (Q16 定点双线性; be=true 输出大端, 与编码前置字节交换融合)      */
/* ================================================================== */
static void scale_impl(const uint16_t *src, uint32_t w_src, uint32_t h_src,
                       uint16_t *dst, uint32_t w_dst, uint32_t h_dst, bool be)
{
    uint32_t x, y;

    if (w_dst == w_src && h_dst == h_src) {
        if (be) {
            hw2d_swap16(dst, src, w_src * h_src);
        } else {
            memcpy(dst, src, w_src * h_src * 2);
        }
        return;
    }
    for (y = 0; y < h_dst; y++) {
        uint32_t sy = (uint32_t)(((uint64_t)y * h_src * 65536u) / h_dst);
        uint32_t y0 = sy >> 16;
        uint32_t fy = sy & 0xFFFFu;
        uint32_t y1 = (y0 + 1 < h_src) ? y0 + 1 : y0;
        const uint16_t *r0 = src + (size_t)y0 * w_src;
        const uint16_t *r1 = src + (size_t)y1 * w_src;
        uint16_t *orow = dst + (size_t)y * w_dst;

        for (x = 0; x < w_dst; x++) {
            uint32_t sx = (uint32_t)(((uint64_t)x * w_src * 65536u) / w_dst);
            uint32_t x0 = sx >> 16;
            uint32_t fx = sx & 0xFFFFu;
            uint32_t x1 = (x0 + 1 < w_src) ? x0 + 1 : x0;
            uint16_t p00 = r0[x0], p01 = r0[x1], p10 = r1[x0], p11 = r1[x1];
            int r, g, b;
            int a0, a1, b0, b1, h0, h1;

            a0 = (p00 >> 11) & 0x1F; a1 = (p01 >> 11) & 0x1F;
            b0 = (p10 >> 11) & 0x1F; b1 = (p11 >> 11) & 0x1F;
            h0 = a0 + (((a1 - a0) * (int)fx) >> 16);
            h1 = b0 + (((b1 - b0) * (int)fx) >> 16);
            r = h0 + (((h1 - h0) * (int)fy) >> 16);

            a0 = (p00 >> 5) & 0x3F; a1 = (p01 >> 5) & 0x3F;
            b0 = (p10 >> 5) & 0x3F; b1 = (p11 >> 5) & 0x3F;
            h0 = a0 + (((a1 - a0) * (int)fx) >> 16);
            h1 = b0 + (((b1 - b0) * (int)fx) >> 16);
            g = h0 + (((h1 - h0) * (int)fy) >> 16);

            a0 = p00 & 0x1F; a1 = p01 & 0x1F;
            b0 = p10 & 0x1F; b1 = p11 & 0x1F;
            h0 = a0 + (((a1 - a0) * (int)fx) >> 16);
            h1 = b0 + (((b1 - b0) * (int)fx) >> 16);
            b = h0 + (((h1 - h0) * (int)fy) >> 16);

            uint16_t px = (uint16_t)((r << 11) | (g << 5) | b);
            orow[x] = be ? (uint16_t)((px >> 8) | (px << 8)) : px;
        }
    }
}

void hw2d_scale(const uint16_t *src, uint32_t w_src, uint32_t h_src,
                uint16_t *dst, uint32_t w_dst, uint32_t h_dst)
{
    scale_impl(src, w_src, h_src, dst, w_dst, h_dst, false);
}

void hw2d_scale_be(const uint16_t *src, uint32_t w_src, uint32_t h_src,
                   uint16_t *dst, uint32_t w_dst, uint32_t h_dst)
{
    scale_impl(src, w_src, h_src, dst, w_dst, h_dst, true);
}

/* ================================================================== */
/* 填充 / 拷贝                                                         */
/* ================================================================== */
void hw2d_fill(uint16_t *buf, uint32_t npix, uint16_t color)
{
    if ((((uintptr_t)buf) & 7u) == 0) {
        uint64_t c8 = (uint64_t)color | ((uint64_t)color << 16) |
                      ((uint64_t)color << 32) | ((uint64_t)color << 48);
        uint32_t n8 = npix / 4;
        uint64_t *q = (uint64_t *)buf;
        uint32_t i;
        for (i = 0; i < n8; i++) {
            q[i] = c8;
        }
        for (i = n8 * 4; i < npix; i++) {
            buf[i] = color;
        }
    } else {
        uint32_t i;
        for (i = 0; i < npix; i++) {
            buf[i] = color;
        }
    }
}

uint32_t hw2d_copy(uint16_t *dst, const uint16_t *src, uint32_t bytes)
{
    uint32_t t0 = HW2D_TIME_US();
    memcpy(dst, src, bytes);
    return HW2D_TIME_US() - t0;
}

/* ================================================================== */
/* 性能统计                                                            */
/* ================================================================== */
static struct {
    uint32_t n_lut;
    uint64_t us_lut;
    uint32_t n_exact;
    uint64_t us_exact;
    uint32_t n_blur;
    uint64_t us_blur;
    uint32_t n_blend;
    uint64_t us_blend;
    uint32_t n_scale;
    uint64_t us_scale;
    uint32_t n_copy;
    uint64_t us_copy;
    uint32_t n_fused;
    uint64_t us_fused;
} s_stats;

void hw2d_stats_reset(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
}

void hw2d_stats_dump(void)
{
    /* 注意: Xtensa 上 uint32_t == unsigned long, %u 会触发 -Werror=format */
    if (s_stats.n_lut) {
        printf("[hw2d] filter_lut : %lu calls, avg %llu us\n",
               (unsigned long)s_stats.n_lut, s_stats.us_lut / s_stats.n_lut);
    }
    if (s_stats.n_exact) {
        printf("[hw2d] filter_exact: %lu calls, avg %llu us\n",
               (unsigned long)s_stats.n_exact, s_stats.us_exact / s_stats.n_exact);
    }
    if (s_stats.n_blur) {
        printf("[hw2d] blur3x3    : %lu calls, avg %llu us\n",
               (unsigned long)s_stats.n_blur, s_stats.us_blur / s_stats.n_blur);
    }
    if (s_stats.n_blend) {
        printf("[hw2d] blend      : %lu calls, avg %llu us\n",
               (unsigned long)s_stats.n_blend, s_stats.us_blend / s_stats.n_blend);
    }
    if (s_stats.n_scale) {
        printf("[hw2d] scale      : %lu calls, avg %llu us\n",
               (unsigned long)s_stats.n_scale, s_stats.us_scale / s_stats.n_scale);
    }
    if (s_stats.n_copy) {
        printf("[hw2d] copy       : %lu calls, avg %llu us\n",
               (unsigned long)s_stats.n_copy, s_stats.us_copy / s_stats.n_copy);
    }
    if (s_stats.n_fused) {
        printf("[hw2d] scale2x_lut: %lu calls, avg %llu us\n",
               (unsigned long)s_stats.n_fused, s_stats.us_fused / s_stats.n_fused);
    }
}

/* ------------------------------------------------------------------ */
/* 带计时的包装 (供 UI 层调用, 便于验证加速收益)                       */
/* ------------------------------------------------------------------ */
void hw2d_filter_lut_stat(uint16_t *dst, const uint16_t *src, uint32_t npix,
                          const hw2d_filter_t *f)
{
    uint32_t t0 = HW2D_TIME_US();
    hw2d_apply_filter_lut(dst, src, npix, f);
    s_stats.n_lut++;
    s_stats.us_lut += HW2D_TIME_US() - t0;
}

void hw2d_filter_exact_stat(uint16_t *dst, const uint16_t *src, uint32_t npix,
                            const hw2d_filter_t *f)
{
    uint32_t t0 = HW2D_TIME_US();
    hw2d_apply_filter_exact(dst, src, npix, f);
    s_stats.n_exact++;
    s_stats.us_exact += HW2D_TIME_US() - t0;
}

void hw2d_blur_stat(uint16_t *out, const uint16_t *in, uint32_t w, uint32_t h,
                    uint8_t strength)
{
    uint32_t t0 = HW2D_TIME_US();
    hw2d_blur3x3(out, in, w, h, strength);
    s_stats.n_blur++;
    s_stats.us_blur += HW2D_TIME_US() - t0;
}

void hw2d_blend_stat(uint16_t *dst, const uint16_t *src, uint32_t npix,
                     uint8_t alpha)
{
    uint32_t t0 = HW2D_TIME_US();
    hw2d_alpha_blend(dst, src, npix, alpha);
    s_stats.n_blend++;
    s_stats.us_blend += HW2D_TIME_US() - t0;
}

void hw2d_scale_stat(const uint16_t *src, uint32_t w_src, uint32_t h_src,
                     uint16_t *dst, uint32_t w_dst, uint32_t h_dst)
{
    uint32_t t0 = HW2D_TIME_US();
    hw2d_scale(src, w_src, h_src, dst, w_dst, h_dst);
    s_stats.n_scale++;
    s_stats.us_scale += HW2D_TIME_US() - t0;
}

void hw2d_scale2x_lut_stat(uint16_t *dst, const uint16_t *src,
                           uint32_t dw, uint32_t dh, const hw2d_filter_t *f)
{
    uint32_t t0 = HW2D_TIME_US();
    hw2d_scale2x_lut(dst, src, dw, dh, f);
    s_stats.n_fused++;
    s_stats.us_fused += HW2D_TIME_US() - t0;
}

esp_err_t hw2d_init(void)
{
    hw2d_stats_reset();

#if HW2D_HAVE_PIE
    if (!s_pie_mtx) s_pie_mtx = xSemaphoreCreateMutex();
    /* PIE swap16 开机自测: 与标量参考逐位比对, 不一致自动回退标量。
     * (盲写汇编的保险丝 —— 真机日志 [EV] simd pie_swap=1 才算生效) */
    static __attribute__((aligned(16))) uint16_t t_src[24];
    static __attribute__((aligned(16))) uint16_t t_pie[24];
    uint16_t t_ref[24];
    for (int i = 0; i < 24; i++) t_src[i] = (uint16_t)(0xA050 + i * 0x0123);
    swap16_c(t_ref, t_src, 24);
    hw2d_pie_swap16_blk(t_pie, t_src, 3);    /* 3 x 8 像素 */
    s_pie_swap_ok = (memcmp(t_pie, t_ref, sizeof(t_ref)) == 0);
    printf("[EV] simd pie_swap=%d\n", (int)s_pie_swap_ok);
#endif
    return ESP_OK;
}
