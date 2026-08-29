#!/bin/zsh
# 烧录全量中文字库到 fonts 分区 (partitions_16mb.csv: ota_1 之后, 偏移 0xC20000)
# 用法: tools/flash_font.sh [port]
set -e
cd "$(dirname "$0")/.."
BIN=build_fonts/cn_full_16.bin
[ -f "$BIN" ] || { echo "先运行 tools/make_cn_font_full.sh"; exit 1; }
P=${1:-$(ls /dev/tty.usbmodem* 2>/dev/null | head -1)}
[ -n "$P" ] || { echo "未找到串口"; exit 1; }
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port "$P" \
  --baud 921600 write_flash 0xC20000 "$BIN"
