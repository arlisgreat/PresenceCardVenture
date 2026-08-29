/* pvc_jpeg_dims.c - JPEG SOF 尺寸解析 (纯 C 无依赖, host 测试直接复用) */
#include "pvc_jpeg.h"

/*
 * 完整性速检: SOI 打头 + 尾部窗口内有 EOI。用于识别"写了一半"的缓存
 * 文件 (掉电/崩溃打断 fwrite): 残缺 JPEG 解码出暗绿色块
 * (缺失 MCU 的 YUV 全零 -> RGB 恰为绿, 真机实证)。
 * 允许 EOI 后至多 tail 窗口的填充字节 (FATFS 簇尾)。
 */
bool pvc_jpeg_intact(const uint8_t *jpg, size_t len)
{
    if (len < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) return false;
    size_t tail = len > 64 ? 64 : len;
    size_t start = len - tail;
    for (size_t i = len - 2; ; i--) {
        if (jpg[i] == 0xFF && jpg[i + 1] == 0xD9) return true;
        if (i == start) break;
    }
    return false;
}

bool pvc_jpeg_dims(const uint8_t *jpg, size_t len, uint32_t *w, uint32_t *h)
{
    size_t i = 2;
    while (i + 9 <= len) {
        if (jpg[i] != 0xFF) { i++; continue; }
        uint8_t m = jpg[i + 1];
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            *h = ((uint32_t)jpg[i + 5] << 8) | jpg[i + 6];
            *w = ((uint32_t)jpg[i + 7] << 8) | jpg[i + 8];
            return true;
        }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD9)) { i += 2; continue; }
        i += 2 + (((uint32_t)jpg[i + 2] << 8) | jpg[i + 3]);   /* 跳过段 */
    }
    return false;
}
