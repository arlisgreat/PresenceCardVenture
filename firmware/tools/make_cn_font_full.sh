#!/bin/sh
# 全量中文字库 (根本解决乱码): CJK 基本区全集 + 标点/全角, 16px 4bpp
# LVGL 二进制格式 -> 烧入独立字体分区 (partitions_16mb.csv "fonts"),
# 启动时载入 PSRAM (main.c lv_binfont_load)。字库源: Noto Sans SC (OFL)。
# 产物: build_fonts/cn_full_16.bin; 烧录: tools/flash_font.sh
set -e
cd "$(dirname "$0")/.."
FONT=${1:-$HOME/Library/Fonts/NotoSansSC-Regular.ttf}
mkdir -p build_fonts
# 注意: 只收 CJK 全集 + 全角区 + ASCII, 其余符号 --symbols 精确点收。
# 不能整块收 Latin-1/通用标点/CJK 标点区(0x3000-0x303F): 上下标、音调
# 组合符等极端字形会把行高包络从 20px 撑到 31px, 状态栏 24px 全裁切
# (真机实证; 单区实测: CJK 17 / 全角 21 / CJK 标点区 31)。
npx --yes lv_font_conv --font "$FONT" \
  -r 0x20-0x7E -r 0x3000 \
  -r 0x4E00-0x9FFF -r 0xFF00-0xFFEF \
  --symbols "、。〈〉《》「」『』【】〔〕·×÷—‘’“”…" \
  --size 16 --bpp 4 --format bin --no-compress \
  -o build_fonts/cn_full_16.bin
ls -l build_fonts/cn_full_16.bin
