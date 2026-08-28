import { EffectsError } from './image.js';

/** GC0308 RGB565 bytes require an explicitly selected endianness; do not guess. */
export function rgb565ToRgba(bytes, width, height, byteOrder) {
  if (!['le', 'be'].includes(byteOrder)) throw new EffectsError('INVALID_OPTIONS', 'RGB565 byte order must be le or be');
  if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || width * height > 640 * 480) throw new EffectsError('INVALID_OPTIONS', 'RGB565 dimensions exceed VGA');
  if (!(bytes instanceof Uint8Array) || bytes.length !== width * height * 2) throw new EffectsError('INVALID_IMAGE', 'RGB565 byte length does not match dimensions');
  const out = new Uint8ClampedArray(width * height * 4);
  for (let i = 0; i < width * height; i++) {
    const value = byteOrder === 'le' ? bytes[2 * i] | bytes[2 * i + 1] << 8 : bytes[2 * i] << 8 | bytes[2 * i + 1];
    const r = value >>> 11, g = value >>> 5 & 63, b = value & 31;
    out[i * 4] = r << 3 | r >>> 2;
    out[i * 4 + 1] = g << 2 | g >>> 4;
    out[i * 4 + 2] = b << 3 | b >>> 2;
    out[i * 4 + 3] = 255;
  }
  return out;
}
