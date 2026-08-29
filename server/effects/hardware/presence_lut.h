#ifndef PRESENCE_LUT_H
#define PRESENCE_LUT_H

#include <stddef.h>
#include <stdint.h>

#define PRESENCE_LUT17_SIZE 17u
#define PRESENCE_LUT17_ENTRIES 4913u

/*
 * Tiny Effects: sampled encoded-sRGB color only. No heap, floating point or I/O.
 * Numeric RGB565 is R[15:11], G[10:5], B[4:0]; convert camera/display byte order
 * before/after this function. No assumption is made about wire endianness.
 *
 * Grid order matches generateRgb565Lut: red is fastest, blue is slowest.
 * Each input channel is rounded to its nearest grid coordinate (0..16).
 * This is quantized nearest-neighbor lookup, NOT trilinear interpolation.
 * For an identity LUT, quantization can change red/blue by one 5-bit level
 * and green by two 6-bit levels. Bypass lookup for original/intensity=0.
 * Grain, spatial glow, vignette, face detection and stickers are not included.
 */
static inline uint16_t presence_lut17_index_rgb565(uint16_t pixel) {
  const uint32_t red = ((((uint32_t)pixel >> 11) & 31u) * 16u + 15u) / 31u;
  const uint32_t green = ((((uint32_t)pixel >> 5) & 63u) * 16u + 31u) / 63u;
  const uint32_t blue = (((uint32_t)pixel & 31u) * 16u + 15u) / 31u;
  return (uint16_t)((blue * 17u + green) * 17u + red);
}

/* A missing/short table returns the input unchanged, without dereferencing it. */
static inline uint16_t presence_lut17_apply_rgb565(uint16_t pixel,
                                                 const uint16_t *lut,
                                                 size_t lut_entries) {
  if (lut == NULL || lut_entries < PRESENCE_LUT17_ENTRIES) return pixel;
  return lut[presence_lut17_index_rgb565(pixel)];
}

#endif /* PRESENCE_LUT_H */
