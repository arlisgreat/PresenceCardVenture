/* 自动生成: tools/make_ccd_lut.py — 勿手改 */
#pragma once
#include <stdint.h>
#include "hw2d.h"

#define CCD_LUT_N 25
#define CCD_CAM_COUNT 4

extern const uint8_t ccd_f100_l3d_start[] asm("_binary_ccd_f100_l3d_start");
extern const uint8_t ccd_z30_l3d_start[] asm("_binary_ccd_z30_l3d_start");
extern const uint8_t ccd_a620_l3d_start[] asm("_binary_ccd_a620_l3d_start");
extern const uint8_t ccd_m532_l3d_start[] asm("_binary_ccd_m532_l3d_start");

typedef struct {
    const char *name;      /* UI 名 */
    const char *api_id;    /* 上报 filter_id */
    const uint8_t *lut;    /* 25^3 RGB888, [b][g][r] */
    uint8_t grain;         /* 颗粒幅度 (Y 噪声峰值) */
    uint8_t grain_hl;      /* 亮部权重 0-100 */
    uint8_t vignette;      /* 暗角强度 0-100 */
    hw2d_filter_t preview; /* 预览 1D 近似 */
} ccd_cam_t;

static const ccd_cam_t k_ccd_cams[CCD_CAM_COUNT] = {
    { "F100", "ccd_f100", ccd_f100_l3d_start, 12, 99, 30, { 25, 110, 95, 98, 99, 102 } },
    { "Z30", "ccd_z30", ccd_z30_l3d_start, 14, 25, 16, { 4, 84, 81, 94, 99, 107 } },
    { "A620", "ccd_a620", ccd_a620_l3d_start, 13, 25, 14, { 15, 97, 109, 91, 99, 110 } },
    { "M532", "ccd_m532", ccd_m532_l3d_start, 12, 30, 20, { 18, 86, 59, 101, 100, 100 } },
};
