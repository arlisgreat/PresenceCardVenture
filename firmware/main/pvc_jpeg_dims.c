/* pvc_jpeg_dims.c - JPEG SOF 尺寸解析 (纯 C 无依赖, host 测试直接复用) */
#include "pvc_jpeg.h"

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
