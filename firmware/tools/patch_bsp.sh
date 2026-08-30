#!/bin/sh
# BSP 本地补丁 (幂等): LCD SPI 40MHz -> 80MHz (ILI9342C 实测可承受;
# 若花屏改 60MHz)。managed_components 重拉后需重跑 (main/CMakeLists
# 配置期自动执行, 手跑亦可)。
H="$(dirname "$0")/../managed_components/espressif__m5stack_core_s3/include/bsp/m5stack_core_s3.h"
grep -q "PVC patch" "$H" 2>/dev/null && exit 0
sed -i '' 's/#define BSP_LCD_PIXEL_CLOCK_HZ     (40 \* 1000 \* 1000)/#define BSP_LCD_PIXEL_CLOCK_HZ     (80 * 1000 * 1000)  \/* PVC patch: 40->80MHz (tools\/patch_bsp.sh) *\//' "$H"
