#!/bin/bash
# QEMU 仿真运行预览/拍照算法自测 (env:qemu-algo)
# 用法: 在 firmware/ 目录执行 test/qemu/run.sh [超时秒, 默认 180]
set -e
TIMEOUT=${1:-180}
BUILD=.pio/build/qemu-algo

# 1. 定位支持 -M esp32s3 的 qemu (必须是 Espressif fork; 上游/homebrew 无此机型)
qemu_ok() { "$1" -M help 2>/dev/null | grep -q "^esp32s3"; }
QEMU=$(ls -d "$HOME"/.espressif/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa 2>/dev/null | tail -1 || true)
if [ -z "$QEMU" ] || ! qemu_ok "$QEMU"; then
  P=$(command -v qemu-system-xtensa || true)
  if [ -n "$P" ] && qemu_ok "$P"; then QEMU=$P; else QEMU=""; fi
fi
if [ -z "$QEMU" ]; then
  IDF_TOOLS=$(ls -d "$HOME"/.platformio/packages/framework-espidf/tools/idf_tools.py 2>/dev/null | tail -1)
  echo "[qemu] installing qemu-xtensa via idf_tools..."
  python3 "$IDF_TOOLS" install qemu-xtensa
  QEMU=$(ls -d "$HOME"/.espressif/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa 2>/dev/null | tail -1)
fi
[ -n "$QEMU" ] && qemu_ok "$QEMU" || { echo "[qemu] 无支持 esp32s3 的 qemu"; exit 2; }
echo "[qemu] using $QEMU"

# 2. 构建 + 合成 16MB flash 镜像
pio run -e qemu-algo >/dev/null
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
python3 "$ESPTOOL" --chip esp32s3 merge_bin -o "$BUILD/flash.bin" \
  --flash_mode dio --flash_size 16MB --fill-flash-size 16MB \
  0x0 "$BUILD/bootloader.bin" 0x8000 "$BUILD/partitions.bin" \
  0x20000 "$BUILD/firmware.bin" >/dev/null

# 3. 运行, 抓 [ALGO_TEST] 结束标记
LOG=$BUILD/qemu.log
rm -f "$LOG"
"$QEMU" -nographic -M esp32s3 -m 4M \
  -drive file="$BUILD/flash.bin",if=mtd,format=raw \
  -serial file:"$LOG" &
QPID=$!
trap 'kill $QPID 2>/dev/null || true' EXIT

for i in $(seq "$TIMEOUT"); do
  if grep -q "\[ALGO_TEST\] ALL PASS" "$LOG" 2>/dev/null; then
    grep "\[ALGO" "$LOG"; echo "[qemu] PASS"; exit 0
  fi
  if grep -qE "\[ALGO_TEST\] FAILED|Guru Meditation|abort\(\)" "$LOG" 2>/dev/null; then
    tail -40 "$LOG"; echo "[qemu] FAILED"; exit 1
  fi
  sleep 1
done
echo "[qemu] TIMEOUT (${TIMEOUT}s)"; tail -40 "$LOG"; exit 1
