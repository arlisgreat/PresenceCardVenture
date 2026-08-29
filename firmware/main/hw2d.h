/*
 * hw2d.h - CoreS3-Lite (ESP32-S3) 硬件 2D 加速层
 *
 * 说明:
 *   ESP32-S3 (Xtensa LX7) 没有 PPA 像素处理加速器 (那是 ESP32-P4 / ESP32-S31
 *   才有的硬件)。本层把 S3 可用的全部硬件 2D 加速路径封装为统一接口:
 *
 *   1. 摄像头 I2S DMA 零拷贝渲染管线 (最省 CPU)
 *      - esp32-camera 采集 DMA 直写 PSRAM 双缓冲, 渲染线程 grab 后直读,
 *        消除每帧 150KB 的 memcpy 搬运。
 *   2. esp_lcd + GDMA + PSRAM 双缓冲刷新 (由 BSP/esp_lvgl_port 完成)
 *      - 全屏 RGB565 刷新通过 LCD SPI DMA 送出, CPU 只做启动传输。
 *   3. PIE 向量指令 / 定点像素算子 (本文件)
 *      - LUT 滤镜 (预览快路径)、精确滤镜 (拍照)、磨皮 3x3、
 *        alpha 混合 (贴纸)、双线性缩放 (相册缩略图)、64bit 填充 (闪光)。
 *      - 全部为整数定点, 无浮点, 无除法热点 (除法用查表)。
 *
 * 所有像素格式: RGB565 小端 (uint16_t), 与 GC0308 (esp32-camera) 输出一致。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(ESP_PLATFORM)
#include "esp_err.h"
#else
typedef int esp_err_t;
#define ESP_OK            0
#define ESP_ERR_NO_MEM    0x101
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 滤镜参数                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    int8_t  bias;       /* 亮度偏置 (美白叠加) -64..64, 0=不变        */
    uint8_t contrast;   /* 对比度 0..200 %, 100=不变                  */
    uint8_t sat;        /* 饱和度 0..200 %, 100=不变                  */
    uint8_t gain_r;     /* 红通道增益 0..200 %                        */
    uint8_t gain_g;     /* 绿通道增益 0..200 %                        */
    uint8_t gain_b;     /* 蓝通道增益 0..200 %                        */
} hw2d_filter_t;

/* 内置滤镜表 (对应 tools/ui_design 的 6 个滤镜) */
typedef enum {
    HW2D_FILTER_ORIGINAL = 0, /* 原图      */
    HW2D_FILTER_FAIR,         /* 白皙      */
    HW2D_FILTER_WARM,         /* 暖阳      */
    HW2D_FILTER_COOL,         /* 冷调      */
    HW2D_FILTER_BW,           /* 黑白      */
    HW2D_FILTER_VINTAGE,      /* 复古      */
    HW2D_FILTER_MAX
} hw2d_filter_id_t;

/* ------------------------------------------------------------------ */
/* 初始化 / 能力                                                       */
/* ------------------------------------------------------------------ */

/* 初始化硬件加速层: 探测能力、预分配滤镜 LUT 等。失败不致命 (自动降级)。 */
esp_err_t hw2d_init(void);

/* 查询某内置滤镜的参数 */
const hw2d_filter_t *hw2d_filter_get(hw2d_filter_id_t id);

/* ------------------------------------------------------------------ */
/* 滤镜 (RGB565)                                                       */
/* ------------------------------------------------------------------ */

/*
 * LUT 快路径 (预览用, ~3-5ms/帧@320x240):
 *   dst = filter(src)。查表按 "灰阶近似" 合成对比/亮度/饱和/增益。
 *   逐 8 像素批次处理, 缓存友好。
 */
void hw2d_apply_filter_lut(uint16_t *dst, const uint16_t *src, uint32_t npix,
                           const hw2d_filter_t *f);

/*
 * 精确路径 (拍照保存用, 每像素独立饱和计算, ~8-12ms/帧):
 *   无 LUT 近似, 逐像素 r/g/b 独立计算, 各步夹取到 [0,255]。
 */
void hw2d_apply_filter_exact(uint16_t *dst, const uint16_t *src, uint32_t npix,
                             const hw2d_filter_t *f);

/* ------------------------------------------------------------------ */
/* 磨皮 3x3 均值 (RGB565)                                              */
/* ------------------------------------------------------------------ */

/*
 * 3x3 box 磨皮。内部把源帧拆为 r/g/b 三个 8bit 平面 (PSRAM 中间缓冲,
 * 需先 hw2d_blur_prepare() 分配), 行间滑窗累加, 均值用 2296 项查表
 * (避免任何除法指令)。out 可与 in 相同 (原地)。
 */
esp_err_t hw2d_blur_prepare(uint32_t w, uint32_t h);
void hw2d_blur3x3(uint16_t *out, const uint16_t *in, uint32_t w, uint32_t h,
                  uint8_t strength); /* strength 0..100, 0=不磨皮(直拷) */
void hw2d_blur_deinit(void);

/* ------------------------------------------------------------------ */
/* Alpha 混合 (贴纸 SRC_OVER)                                          */
/* ------------------------------------------------------------------ */

/*
 * dst = (src*alpha + dst*(255-alpha)) >> 8, 通道域 int16 定点, 无溢出。
 * 逐 8 像素批次。src/dst 均为 RGB565。
 */
void hw2d_alpha_blend(uint16_t *dst, const uint16_t *src, uint32_t npix,
                      uint8_t alpha);

/* ------------------------------------------------------------------ */
/* 缩放 (整数定点双线性, 相册缩略图/全屏查看)                          */
/* ------------------------------------------------------------------ */

/*
 * 从 src[w_src x h_src] 双线性缩放到 dst[w_dst x h_dst]。
 * 内部 Q16 定点, 无浮点。src/dst 可重叠? 不能, 必须分离缓冲。
 */
void hw2d_scale(const uint16_t *src, uint32_t w_src, uint32_t h_src,
                uint16_t *dst, uint32_t w_dst, uint32_t h_dst);

/*
 * 同 hw2d_scale, 但输出直接写为大端字节序 (JPEG 编码器/相机原生序)。
 * 与缩放融合为单遍, 消除独立字节交换 pass (JPEG 编码前置处理专用)。
 */
void hw2d_scale_be(const uint16_t *src, uint32_t w_src, uint32_t h_src,
                   uint16_t *dst, uint32_t w_dst, uint32_t h_dst);

/*
 * 预览融合算子: 2:1 盒均值降采样 + LUT 滤镜单遍 (src 恰为 2dw x 2dh)。
 * 共享 LUT 静态表, 仅限单任务 (预览/LVGL) 调用。
 */
void hw2d_scale2x_lut(uint16_t *dst, const uint16_t *src,
                      uint32_t dw, uint32_t dh, const hw2d_filter_t *f);
void hw2d_scale2x_lut_stat(uint16_t *dst, const uint16_t *src,
                           uint32_t dw, uint32_t dh, const hw2d_filter_t *f);

/* ------------------------------------------------------------------ */
/* YUV422 (YCbYCr packed) 算子 —— 全链路 YUV 架构                       */
/*   采集/滤镜/磨皮/编码全程留在传感器原生 YUV 域 (零 RGB565 量化损失), */
/*   RGB565 仅作为显示边界。字节序: Y0 Cb Y1 Cr (GC0308 寄存器注释 =    */
/*   esp_new_jpeg 的 JPEG_PIXEL_FORMAT_YCbYCr, 直通编码器)。            */
/*   约定: npix 为像素数且必须为偶数 (两像素共享一组色度)。             */
/* ------------------------------------------------------------------ */

/*
 * YUV 域滤镜查表 (调用方持有, 消除跨核共享静态表竞争):
 *   y[]  承载 亮度偏置+对比度+综合增益; cb[]/cr[] 承载 饱和度缩放+色调偏移
 *   (暖/冷调由 gain_r/gain_b 差值映射为 Cr/Cb 偏移)。sat=0 时 cb/cr 恒 128。
 */
typedef struct {
    uint8_t y[256];
    uint8_t cb[256];
    uint8_t cr[256];
    hw2d_filter_t cached;    /* 参数快照, hw2d_yuv_build_luts 按需重建 */
    bool valid;
} hw2d_yuv_luts_t;

/* 按 f 重建 luts (与 cached 相同则跳过); luts 由调用方零初始化 */
void hw2d_yuv_build_luts(const hw2d_filter_t *f, hw2d_yuv_luts_t *luts);

/* YUYV -> YUYV 滤镜 (拍照链); dst 可等于 src (原地) */
void hw2d_yuv_filter(uint8_t *dst, const uint8_t *src, uint32_t npix,
                     const hw2d_yuv_luts_t *luts);

/* YUYV -> RGB565(LE) 滤镜+转换融合单遍 (预览显示边界; BT.601 全范围) */
void hw2d_yuv_filter_rgb565(uint16_t *dst, const uint8_t *src, uint32_t npix,
                            const hw2d_yuv_luts_t *luts);
void hw2d_yuv_filter_rgb565_stat(uint16_t *dst, const uint8_t *src,
                                 uint32_t npix, const hw2d_yuv_luts_t *luts);

/* 反向写出变体: 输出即 180 度旋转 (GC0308 CISCTL 翻转位真机写不进,
 * 装配方向由软件补偿; 预览零额外遍历) */
void hw2d_yuv_filter_rgb565_rot180(uint16_t *dst, const uint8_t *src,
                                   uint32_t npix, const hw2d_yuv_luts_t *luts);
void hw2d_yuv_filter_rgb565_rot180_stat(uint16_t *dst, const uint8_t *src,
                                        uint32_t npix,
                                        const hw2d_yuv_luts_t *luts);

/* YUYV422 原地 180 度旋转 (拍照链: 快照进 worker 先转再处理, ~2ms) */
void hw2d_yuv_rot180(uint8_t *yuyv, uint32_t npix);

/* YUYV 磨皮: 仅 3x3 平滑 Y 平面 (经典磨皮: 平亮度保色度, 算量 1/3)。
 * dst 可等于 src; 内部借用 blur 平面缓冲 (单任务调用, 与 blur3x3 互斥) */
void hw2d_yuv_blur_y(uint8_t *dst, const uint8_t *src, uint32_t w, uint32_t h,
                     uint8_t strength);

/* YUYV 提亮混合圆 (贴纸合成进照片): 圆内 Y 向 255、色度向 128 按 alpha 靠拢;
 * 圆心/半径超出画面时整圆裁剪 */
void hw2d_yuv_blend_circle(uint8_t *yuyv, uint32_t w, uint32_t h,
                           int cx, int cy, int r, uint8_t alpha);

/* YUYV 提取 Y 平面 (人脸检测 GRAY 输入等) */
void hw2d_yuv_extract_y(uint8_t *dst_gray, const uint8_t *src, uint32_t npix);

/* ------------------------------------------------------------------ */
/* CCD 机型滤镜 (拍照链专用, 全保真路径)                                */
/* ------------------------------------------------------------------ */

/*
 * 三维 LUT 调色 (原地): 逐像素 YUV->RGB (BT.601) -> N^3 三线性查表
 * -> RGB->YUV 写回。lut 为 [b][g][r][rgb] 序 RGB888, 每轴 n 采样。
 * QVGA 约 100-200ms, 仅在 worker 拍照链使用 (预览走 1D 近似参数)。
 */
void hw2d_yuv_3dlut(uint8_t *yuyv, uint32_t npix, const uint8_t *lut, int n);

/* 胶片颗粒 (原地, 仅加于 Y): amount=噪声峰值, highlights=亮部权重 0-100,
 * seed 决定颗粒图案 (同 seed 可复现) */
void hw2d_yuv_grain(uint8_t *yuyv, uint32_t w, uint32_t h,
                    uint8_t amount, uint8_t highlights, uint32_t seed);

/* 暗角 (原地, 仅衰减 Y): strength 0-100, 边角处 Y 衰减至 (100-strength)% */
void hw2d_yuv_vignette(uint8_t *yuyv, uint32_t w, uint32_t h, uint8_t strength);

/* ------------------------------------------------------------------ */
/* 16bit 车道字节交换 (RGB565 小端 <-> 大端)                            */
/* ------------------------------------------------------------------ */

/*
 * dst[i] = byteswap16(src[i])。允许 dst==src 原地转换
 * (各路径均为读后写同址); 部分重叠不允许。
 * ESP32-S3 上若 PIE 128-bit 路径可用 (开机自测通过) 且两缓冲 16 字节对齐,
 * 走 PIE 汇编 (8 像素/拍); 否则 64bit 掩码路径 (4 像素/次)。
 */
void hw2d_swap16(uint16_t *dst, const uint16_t *src, uint32_t npix);

/* PIE 加速是否生效 (hw2d_init 自测决定; 埋点用) */
bool hw2d_pie_active(void);

/* ------------------------------------------------------------------ */
/* 填充 (闪光动画等)                                                   */
/* ------------------------------------------------------------------ */

/* 64bit 宽写填充, 比逐像素 memset 快 ~8x */
void hw2d_fill(uint16_t *buf, uint32_t npix, uint16_t color);

/* ------------------------------------------------------------------ */
/* 缓冲拷贝 (零拷贝管线关闭时的后备)                                   */
/* ------------------------------------------------------------------ */

/* PSRAM 到 PSRAM / SRAM 之间对齐拷贝。返回实际耗时 us (便于统计)。 */
uint32_t hw2d_copy(uint16_t *dst, const uint16_t *src, uint32_t bytes);

/* ------------------------------------------------------------------ */
/* 性能统计 (验证硬件加速收益用)                                       */
/* ------------------------------------------------------------------ */
void hw2d_stats_reset(void);
void hw2d_stats_dump(void);

/*
 * 带计时统计的调用包装 (UI 层使用这些, 便于验证每帧 CPU 开销):
 *   等价于对应裸函数, 仅多累加一次 esp_timer 计数。
 */
void hw2d_filter_lut_stat(uint16_t *dst, const uint16_t *src, uint32_t npix,
                          const hw2d_filter_t *f);
void hw2d_filter_exact_stat(uint16_t *dst, const uint16_t *src, uint32_t npix,
                            const hw2d_filter_t *f);
void hw2d_blur_stat(uint16_t *out, const uint16_t *in, uint32_t w, uint32_t h,
                    uint8_t strength);
void hw2d_blend_stat(uint16_t *dst, const uint16_t *src, uint32_t npix,
                     uint8_t alpha);
void hw2d_scale_stat(const uint16_t *src, uint32_t w_src, uint32_t h_src,
                     uint16_t *dst, uint32_t w_dst, uint32_t h_dst);

#ifdef __cplusplus
}
#endif
