#!/bin/sh
# 全量中文字库 (根本解决乱码): CJK 基本区全集 + 标点/全角, 16px 4bpp
# LVGL 二进制格式 -> 烧入独立字体分区 (partitions_16mb.csv "fonts"),
# 启动时载入 PSRAM (main.c lv_binfont_load)。字库源: Noto Sans SC (OFL)。
# 产物: build_fonts/cn_full_16.bin; 烧录: tools/flash_font.sh
set -e
cd "$(dirname "$0")/.."
FONT=${1:-$HOME/Library/Fonts/NotoSansSC-Regular.ttf}
mkdir -p build_fonts
npx --yes lv_font_conv --font "$FONT" \
  -r 0x20-0x7E -r 0xA0-0xFF -r 0x2000-0x206F -r 0x3000-0x303F \
  -r 0x4E00-0x9FFF -r 0xFF00-0xFFEF \
  --size 16 --bpp 4 --format bin --no-compress \
  -o build_fonts/cn_full_16.bin
ls -l build_fonts/cn_full_16.bin
