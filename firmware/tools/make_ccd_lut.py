# -*- coding: utf-8 -*-
"""CCD 滤镜资产管线: ProCCD 逆向 square-LUT -> 固件 25^3 三维 LUT + 预览近似参数。

输入: assets/ccd/ 下 4 台机型的 512x512 square-LUT (GPUImage 布局:
      蓝色选 8x8 分块, 块内 x=红 y=绿 —— ccd_real_lut.py 验证过的正确布局)
输出: main/ccd_assets/ccd_<id>.l3d   25*25*25*3 RGB888 二进制 (R 最快变化)
      main/ccd_assets/ccd_luts.h     机型表 (embed 符号 + 颗粒/暗角 + 预览近似参数)

预览近似: 沿灰轴与彩色采样点拟合 hw2d_filter_t (bias/contrast/sat/gain),
取景器色调接近、成片用全 LUT 保真。
用法: 在 firmware/ 目录 python3 tools/make_ccd_lut.py
"""
import os
import numpy as np
from PIL import Image

N = 25                       # 立方 LUT 每轴采样数
A = os.path.join(os.path.dirname(__file__), "..", "..", "assets", "ccd")
OUT = os.path.join(os.path.dirname(__file__), "..", "main", "ccd_assets")

# (id, 显示名, LUT 文件, 颗粒幅度0-255域, 亮部权重, 暗角强度0-100)
CAMERAS = [
    ("f100", "F100", "f100_filter_c.png", 12, 99, 30),
    ("z30",  "Z30",  "lut_01.jpg",        14, 25, 16),
    ("a620", "A620", "lut_02.jpg",        13, 25, 14),
    ("m532", "M532", "lut_03.jpg",        12, 30, 20),
]


def fetch(lut, rk, gk, bk):
    """square-LUT 取样 (64 级索引): 蓝选块, 块内 x=红 y=绿"""
    x = (bk % 8) * 64 + rk
    y = (bk // 8) * 64 + gk
    return lut[y, x]


def sample_cube(lut):
    """512 方图 -> N^3 立方 (三线性于 64 级源网格)"""
    cube = np.zeros((N, N, N, 3), np.float32)      # 索引序 [b][g][r]
    idx = np.linspace(0.0, 63.0, N)
    for bi, bv in enumerate(idx):
        b0 = int(bv); b1 = min(b0 + 1, 63); fb = bv - b0
        for gi, gv in enumerate(idx):
            g0 = int(gv); g1 = min(g0 + 1, 63); fg = gv - g0
            for ri, rv in enumerate(idx):
                r0 = int(rv); r1 = min(r0 + 1, 63); fr = rv - r0
                acc = np.zeros(3, np.float32)
                for bb, wb in ((b0, 1 - fb), (b1, fb)):
                    for gg, wg in ((g0, 1 - fg), (g1, fg)):
                        for rr, wr in ((r0, 1 - fr), (r1, fr)):
                            acc += fetch(lut, rr, gg, bb) * (wr * wg * wb)
                cube[bi, gi, ri] = acc
    return np.clip(cube, 0, 255)


def fit_preview(cube):
    """拟合预览近似参数 -> (bias, contrast, sat, gr, gg, gb) 各按 hw2d 语义"""
    g = np.array([cube[i, i, i] for i in range(N)])          # 灰轴响应
    lum_in = np.linspace(0, 255, N)
    lum_out = g.mean(1)
    k = np.polyfit(lum_in[3:-3], lum_out[3:-3], 1)           # 掐头去尾拟斜率
    contrast = int(round(np.clip(k[0], 0.5, 2.0) * 100))
    bias = int(round(np.clip(k[1] + (k[0] - 1) * 128, -64, 64)))
    gains = g[N // 2] / max(1e-3, g[N // 2].mean())          # 中灰通道偏
    gr, gg_, gb = (int(round(np.clip(x * 100, 50, 150))) for x in gains)
    # 饱和: 取若干彩色采样点, 比较输出/输入的色度幅度
    pts = [(20, 8, 8), (8, 20, 8), (8, 8, 20), (22, 14, 6), (6, 14, 22)]
    rin, rout = [], []
    for (ri, gi, bi) in pts:
        i_rgb = np.array([ri, gi, bi]) * 255.0 / (N - 1)
        o_rgb = cube[bi, gi, ri]
        rin.append(np.std(i_rgb)); rout.append(np.std(o_rgb))
    sat = int(round(np.clip(np.mean(rout) / max(1e-3, np.mean(rin)), 0.3, 1.5) * 100))
    return bias, contrast, sat, gr, gg_, gb


def main():
    os.makedirs(OUT, exist_ok=True)
    rows = []
    for cid, name, fn, grain, hl, vig in CAMERAS:
        lut = np.asarray(Image.open(os.path.join(A, fn)).convert("RGB"), np.float32)
        assert lut.shape == (512, 512, 3), f"{fn}: {lut.shape}"
        cube = sample_cube(lut)
        path = os.path.join(OUT, f"ccd_{cid}.l3d")
        cube.astype(np.uint8).tofile(path)               # [b][g][r][rgb]
        bias, con, sat, gr, gg_, gb = fit_preview(cube)
        rows.append((cid, name, grain, hl, vig, bias, con, sat, gr, gg_, gb))
        print(f"{cid}: {os.path.getsize(path)}B preview="
              f"(bias={bias} con={con} sat={sat} gain={gr}/{gg_}/{gb})")

    with open(os.path.join(OUT, "ccd_luts.h"), "w") as f:
        f.write("/* 自动生成: tools/make_ccd_lut.py — 勿手改 */\n")
        f.write("#pragma once\n#include <stdint.h>\n#include \"hw2d.h\"\n\n")
        f.write(f"#define CCD_LUT_N {N}\n#define CCD_CAM_COUNT {len(rows)}\n\n")
        for cid, *_ in rows:
            f.write(f"extern const uint8_t ccd_{cid}_l3d_start[] "
                    f"asm(\"_binary_ccd_{cid}_l3d_start\");\n")
        f.write("\ntypedef struct {\n    const char *name;      /* UI 名 */\n"
                "    const char *api_id;    /* 上报 filter_id */\n"
                "    const uint8_t *lut;    /* 25^3 RGB888, [b][g][r] */\n"
                "    uint8_t grain;         /* 颗粒幅度 (Y 噪声峰值) */\n"
                "    uint8_t grain_hl;      /* 亮部权重 0-100 */\n"
                "    uint8_t vignette;      /* 暗角强度 0-100 */\n"
                "    hw2d_filter_t preview; /* 预览 1D 近似 */\n"
                "} ccd_cam_t;\n\n")
        f.write("static const ccd_cam_t k_ccd_cams[CCD_CAM_COUNT] = {\n")
        for cid, name, grain, hl, vig, bias, con, sat, gr, gg_, gb in rows:
            f.write(f'    {{ "{name}", "ccd_{cid}", ccd_{cid}_l3d_start, '
                    f"{grain}, {hl}, {vig}, "
                    f"{{ {bias}, {con}, {sat}, {gr}, {gg_}, {gb} }} }},\n")
        f.write("};\n")
    print("wrote ccd_luts.h")


if __name__ == "__main__":
    main()
