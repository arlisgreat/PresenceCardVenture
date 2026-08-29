# -*- coding: utf-8 -*-
"""正确使用 ProCCD 逆向得到的 6 台相机真实 LUT。

关键修正: LUT 是 512x512 的 "square lookup table" (GPUImage LookupFilter 布局):
  蓝色选 8x8 分块(tile), 块内 x=红 y=绿。
  之前 render_all.py 用 red 选块 -> R/B 反相 -> 皮肤发蓝(final_luts.jpg 的错误)。
本脚本用正确布局 + 三线性插值, 并叠加各机型 ProCCD 真实颗粒/暗角/辉光参数。
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

RE = "/tmp/ccd_re"
SRC = "input_1.jpg"
OUT = "ccd_real_lut_2x3.jpg"

# 6 台相机: (显示名, LUT 路径, 参数)
CAMERAS = [
    ("富士 F100fd", f"{RE}/proccd_real/ext3/assets/camera_res/render_res/lut/camera_ab_test/f100_filter_c.png",
     dict(grain=dict(size=0.1369, amount=0.1124, highlights=0.99, roughness=0.44, alpha=0.72),
          vignette=0.30, inner=0.32, outer=0.62, hg=0.22, hg_rgb=(0.881, 0.870, 0.877), struct=0.15)),
    ("佳能 A620", f"{RE}/luts/lut_02.jpg",
     dict(grain=dict(size=0.10, amount=0.12, highlights=0.25, roughness=0.5, alpha=1.0),
          vignette=0.14, struct=0.30)),
    ("富士 Z30", f"{RE}/luts/lut_01.jpg",
     dict(grain=dict(size=0.11, amount=0.13, highlights=0.25, roughness=0.5, alpha=1.0),
          vignette=0.16, struct=0.0)),
    ("柯达 M532", f"{RE}/luts/lut_03.jpg",
     dict(grain=dict(size=0.10, amount=0.11, highlights=0.3, roughness=0.5, alpha=1.0),
          vignette=0.20, struct=0.1)),
    ("富士 F100 夜景", f"{RE}/proccd_real/ext3/assets/camera_res/render_res/lut/camera_ab_test/f100_filter_b.png",
     dict(grain=dict(size=0.13, amount=0.11, highlights=0.9, roughness=0.44, alpha=0.7),
          vignette=0.34, inner=0.30, outer=0.55, hg=0.28, hg_rgb=(0.85, 0.87, 0.9), struct=0.15)),
    ("机型 #4 (捕获)", f"{RE}/luts/lut_04.png",
     dict(grain=dict(size=0.10, amount=0.10, highlights=0.25, roughness=0.5, alpha=1.0),
          vignette=0.20, struct=0.1)),
]


# ---------- 正确的 square-LUT 三线性采样 ----------
def load_lut(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)  # 512x512x3


def _fetch(lut, rk, gk, bk):
    """按 GPUImage 布局取样: 蓝选块, 块内 x=红 y=绿。rk,gk,bk 为 [0,63] 整型数组。"""
    x = (bk % 8) * 64 + rk
    y = (bk // 8) * 64 + gk
    return lut[y, x]  # -> (...,3) float


def apply_lut(img, lut, intensity=1.0):
    """img float HxWx3 0-255。三线性插值(红/绿/蓝三轴)。"""
    c = np.clip(img / 255.0 * 63.0, 0, 63)
    r0 = np.floor(c[..., 0]).astype(np.int32); fr = c[..., 0] - r0
    g0 = np.floor(c[..., 1]).astype(np.int32); fg = c[..., 1] - g0
    b0 = np.floor(c[..., 2]).astype(np.int32); fb = c[..., 2] - b0
    r1 = np.minimum(r0 + 1, 63); g1 = np.minimum(g0 + 1, 63); b1 = np.minimum(b0 + 1, 63)

    out = np.zeros_like(img)
    for br, wb in ((b0, 1 - fb), (b1, fb)):
        for gr, wg in ((g0, 1 - fg), (g1, fg)):
            for rr, wr in ((r0, 1 - fr), (r1, fr)):
                w = (wr * wg * wb)[..., None]
                out += _fetch(lut, rr, gr, br) * w
    return img * (1 - intensity) + out * intensity


# ---------- ProCCD 算子 ----------
def pro_grain(img, size=0.1, amount=0.11, highlights=0.25, roughness=0.5, alpha=1.0, seed=7):
    H, W = img.shape[:2]
    rng = np.random.default_rng(seed)
    scale = max(1, int(1 + (1.0 - size) * 6))
    gh, gw = max(1, H // scale), max(1, W // scale)
    n = rng.standard_normal((gh, gw)).astype(np.float32)
    n = np.asarray(Image.fromarray(((n - n.min()) / (np.ptp(n) + 1e-6) * 255).astype(np.uint8))
                   .resize((W, H), Image.BILINEAR), np.float32) / 255.0 * 2 - 1
    fine = rng.standard_normal((H, W)).astype(np.float32)
    n = n * (1 - roughness * 0.5) + fine * (roughness * 0.5)
    lum = img.mean(2) / 255.0
    w = 1.0 + highlights * (lum - 0.5)   # 亮部颗粒
    return img + n[..., None] * (amount * 42.0 * alpha) * w[..., None]


def detail(img, s):
    if s <= 0:
        return img
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    blur = np.asarray(im.filter(ImageFilter.GaussianBlur(2)), np.float32)
    return np.clip(img + (img - blur) * (s * 1.6), 0, 255)


def hui_guang(img, intensity, rgb=(1, 1, 1), scattering=3.5, min_t=0.26, max_t=0.99):
    if intensity <= 0:
        return img
    lum = img.mean(2) / 255.0
    t = np.clip((lum - min_t) / max(1e-3, max_t - min_t), 0, 1)
    blur = np.asarray(Image.fromarray((t * 255).astype(np.uint8))
                      .filter(ImageFilter.GaussianBlur(max(1, int(scattering * 3)))), np.float32) / 255.0
    glow = np.stack([blur * rgb[0], blur * rgb[1], blur * rgb[2]], 2) * 255.0
    return np.clip(img + glow * intensity, 0, 255)


def vignette(img, intensity, inner=0.3, outer=0.9):
    if intensity <= 0:
        return img
    H, W = img.shape[:2]
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    d = np.sqrt(((xx - W / 2) / (W / 2)) ** 2 + ((yy - H / 2) / (H / 2)) ** 2) / np.sqrt(2)
    t = np.clip((d - inner) / max(1e-3, outer - inner), 0, 1)
    return img * (1.0 - intensity * t * t)[..., None]


def render(img, lut, p):
    x = apply_lut(img, lut, 1.0)
    x = detail(x, p.get("struct", 0.0))
    g = p.get("grain")
    if g:
        x = pro_grain(x, **g)
    if p.get("hg", 0):
        x = hui_guang(x, p["hg"], p.get("hg_rgb", (1, 1, 1)))
    x = vignette(x, p.get("vignette", 0.0), p.get("inner", 0.3), p.get("outer", 0.9))
    return np.clip(x, 0, 255).astype(np.uint8)


# ---------- 合成 2x3 ----------
src = Image.open(SRC).convert("RGB")
CELL_W = 640
cell_h = round(src.height * CELL_W / src.width)
base = np.asarray(src.resize((CELL_W, cell_h), Image.LANCZOS), dtype=np.float32)

GAP = 8
grid = Image.new("RGB", (CELL_W * 3 + GAP * 4, cell_h * 2 + GAP * 3), (16, 16, 16))
font = ImageFont.truetype("/System/Library/Fonts/STHeiti Medium.ttc", 32)
sub = ImageFont.truetype("/System/Library/Fonts/STHeiti Light.ttc", 20)
stamp = ImageFont.truetype("/System/Library/Fonts/Supplemental/Courier New Bold.ttf", 26)

for i, (name, lp, p) in enumerate(CAMERAS):
    lut = load_lut(lp)
    cell = Image.fromarray(render(base.copy(), lut, p))
    d = ImageDraw.Draw(cell, "RGBA")
    tw = d.textlength(name, font=font)
    d.rectangle([0, cell_h - 84, tw + 34, cell_h], fill=(0, 0, 0, 140))
    d.text((17, cell_h - 76, ), name, font=font, fill=(255, 255, 255, 245))
    d.text((18, cell_h - 38), "ProCCD 真实 LUT", font=sub, fill=(120, 230, 150, 235))
    d.text((CELL_W - 205, cell_h - 44), "26 8 28", font=stamp,
           fill=(255, 150, 40, 220), stroke_width=1, stroke_fill=(150, 70, 0, 120))
    r, c = divmod(i, 3)
    grid.paste(cell, (GAP + c * (CELL_W + GAP), GAP + r * (cell_h + GAP)))

grid.save(OUT, quality=92)
print("saved", OUT, grid.size)
