#ifndef PRESENCE_OVERLAY_H
#define PRESENCE_OVERLAY_H

#include <stddef.h>
#include <stdint.h>

#define PRESENCE_OVERLAY_FRAME_MAX_WIDTH 640u
#define PRESENCE_OVERLAY_FRAME_MAX_HEIGHT 480u
#define PRESENCE_OVERLAY_MAX_WIDTH 128u
#define PRESENCE_OVERLAY_MAX_HEIGHT 128u

/*
 * Tiny Effects: numeric RGB565 words, R[15:11], G[10:5], B[4:0].
 * Convert camera/display byte order outside these functions; do not cast a
 * wire byte buffer to uint16_t*. No heap, floating point, tracking or I/O.
 * Alpha is separate, straight (NOT premultiplied), 0..255.
 * Blend each encoded 5/6/5 channel with integer rounding; NOT linear-light
 * compositing. Alpha 0 preserves the destination; alpha 255 copies exactly.
 */
static inline uint16_t presence_overlay_blend_rgb565(uint16_t background,
                                                     uint16_t foreground,
                                                     uint8_t alpha) {
  if (alpha == 0u) return background;
  if (alpha == 255u) return foreground;
  const uint32_t a = alpha;
  const uint32_t inverse = 255u - a;
  const uint32_t red = ((((uint32_t)foreground >> 11) * a) +
                        (((uint32_t)background >> 11) * inverse) + 127u) / 255u;
  const uint32_t green = (((((uint32_t)foreground >> 5) & 63u) * a) +
                          ((((uint32_t)background >> 5) & 63u) * inverse) + 127u) / 255u;
  const uint32_t blue = ((((uint32_t)foreground & 31u) * a) +
                         (((uint32_t)background & 31u) * inverse) + 127u) / 255u;
  return (uint16_t)((red << 11) | (green << 5) | blue);
}

/*
 * Dense row-major buffers, top to bottom; no row padding. Counts must match
 * dimensions EXACTLY: frame_words/overlay_words count uint16_t elements,
 * alpha_bytes counts uint8_t elements. All buffers must be non-null, valid
 * for their declared lengths; overlay and alpha must not overlap frame.
 * x/y locate the overlay's top-left, with clipping on all four frame edges.
 * Returns 1 for a valid call (including fully off-screen), 0 for invalid
 * dimensions/counts/pointers. Invalid calls never modify the frame.
 */
static inline int presence_overlay_apply_rgb565(uint16_t *frame,
                                                size_t frame_words,
                                                size_t frame_width,
                                                size_t frame_height,
                                                const uint16_t *overlay,
                                                size_t overlay_words,
                                                const uint8_t *alpha,
                                                size_t alpha_bytes,
                                                size_t overlay_width,
                                                size_t overlay_height,
                                                int32_t x,
                                                int32_t y) {
  if (frame == NULL || overlay == NULL || alpha == NULL ||
      frame_width == 0u || frame_width > PRESENCE_OVERLAY_FRAME_MAX_WIDTH ||
      frame_height == 0u || frame_height > PRESENCE_OVERLAY_FRAME_MAX_HEIGHT ||
      overlay_width == 0u || overlay_width > PRESENCE_OVERLAY_MAX_WIDTH ||
      overlay_height == 0u || overlay_height > PRESENCE_OVERLAY_MAX_HEIGHT) return 0;
  if (frame_height > SIZE_MAX / frame_width ||
      overlay_height > SIZE_MAX / overlay_width) return 0;
  if (frame_words != frame_width * frame_height ||
      overlay_words != overlay_width * overlay_height ||
      alpha_bytes != overlay_words) return 0;

  /* Reject extreme coordinates before negation/addition, including INT32_MIN. */
  if (x >= (int32_t)frame_width || y >= (int32_t)frame_height ||
      x <= -(int32_t)overlay_width || y <= -(int32_t)overlay_height) return 1;
  const size_t source_x = x < 0 ? (size_t)(-x) : 0u;
  const size_t source_y = y < 0 ? (size_t)(-y) : 0u;
  const size_t target_x = x > 0 ? (size_t)x : 0u;
  const size_t target_y = y > 0 ? (size_t)y : 0u;
  size_t width = overlay_width - source_x;
  size_t height = overlay_height - source_y;
  if (width > frame_width - target_x) width = frame_width - target_x;
  if (height > frame_height - target_y) height = frame_height - target_y;
  for (size_t row = 0; row < height; ++row) {
    const size_t source = (source_y + row) * overlay_width + source_x;
    const size_t target = (target_y + row) * frame_width + target_x;
    for (size_t column = 0; column < width; ++column) {
      frame[target + column] = presence_overlay_blend_rgb565(
        frame[target + column], overlay[source + column], alpha[source + column]);
    }
  }
  return 1;
}

#endif /* PRESENCE_OVERLAY_H */
