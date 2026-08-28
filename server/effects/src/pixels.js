import { getPreset } from './presets.js';
export { PRESET_VERSION, PRESETS, getPreset } from './presets.js';

const LUMA = [0.2126, 0.7152, 0.0722];
const clamp = (value, low = 0, high = 1) => Math.min(high, Math.max(low, value));
const mix = (start, end, amount) => start + (end - start) * amount;

function validateIntensity(intensity) {
  if (!Number.isFinite(intensity) || intensity < 0 || intensity > 1) {
    throw new RangeError('intensity must be a finite number between 0 and 1');
  }
}

function smoothstep(low, high, value) {
  const x = clamp((value - low) / (high - low));
  return x * x * (3 - 2 * x);
}

function tone(value, curve) {
  const x = clamp(value);
  for (let i = 1; i < curve.length; i++) {
    const [rightX, rightY] = curve[i];
    if (x <= rightX) {
      const [leftX, leftY] = curve[i - 1];
      return mix(leftY, rightY, (x - leftX) / (rightX - leftX));
    }
  }
  return curve[curve.length - 1][1];
}

const TONE_STEPS = 4096;
const toneTables = new Map();

function toneTable(preset) {
  let table = toneTables.get(preset.id);
  if (!table) {
    table = new Float32Array(TONE_STEPS + 1);
    for (let i = 0; i <= TONE_STEPS; i++) table[i] = tone(i / TONE_STEPS, preset.toneCurve);
    toneTables.set(preset.id, table);
  }
  return table;
}

function tableTone(value, table) {
  const position = clamp(value) * TONE_STEPS;
  const left = Math.floor(position);
  if (left === TONE_STEPS) return table[left];
  return mix(table[left], table[left + 1], position - left);
}

/** Unquantized global transform; all spatial operations are deliberately separate. */
function colorTransform(r, g, b, preset, table, output) {
  const luminance = (r * LUMA[0] + g * LUMA[1] + b * LUMA[2]) / 255;
  const shadows = 1 - smoothstep(0.05, 0.50, luminance);
  const highlights = smoothstep(0.60, 1, luminance);
  for (let i = 0; i < 3; i++) {
    const channel = (i === 0 ? r : i === 1 ? g : b) / 255;
    const saturated = mix(luminance, channel, preset.saturation);
    const curved = tableTone(saturated, table) * preset.channelGain[i];
    output[i] = 255 * clamp(curved + preset.shadowTint[i] * shadows + preset.highlightTint[i] * highlights);
  }
  return output;
}

/**
 * Transform a three-channel sRGB sample. Returns a new, unquantized 0–255 tuple.
 * Includes color only: grain, vignette and highlight glow cannot be represented by a LUT.
 */
export function transformRgb(rgb, presetId = 'none', intensity = 1) {
  const preset = getPreset(presetId);
  validateIntensity(intensity);
  if (!rgb || rgb.length !== 3 || ![rgb[0], rgb[1], rgb[2]].every((value) => Number.isFinite(value) && value >= 0 && value <= 255)) {
    throw new RangeError('rgb must contain exactly three finite 0–255 channel values');
  }
  const source = [rgb[0], rgb[1], rgb[2]];
  if (preset.id === 'none' || intensity === 0) return source;
  return colorTransform(...source, preset, toneTable(preset), [0, 0, 0]).map((value, i) => mix(source[i], value, intensity));
}

// Mulberry32: deterministic across JavaScript runtimes. The safe-integer seed is
// interpreted modulo 2^32. This is visual noise, not cryptographic randomness.
function randomSource(seed) {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6D2B79F5) >>> 0;
    let value = Math.imul(state ^ (state >>> 15), state | 1);
    value ^= value + Math.imul(value ^ (value >>> 7), value | 61);
    return ((value ^ (value >>> 14)) >>> 0) / 4294967296;
  };
}

function highlightMask(data, offset, threshold) {
  const luminance = (data[offset] * LUMA[0] + data[offset + 1] * LUMA[1] + data[offset + 2] * LUMA[2]) / 255;
  return smoothstep(threshold, 1, luminance) * (data[offset + 3] / 255);
}

/** Separable, clamped-edge box blur; O(width * height), independent of blur radius. */
function blurredHighlights(data, width, height, glow) {
  const radius = Math.min(16, Math.max(1, Math.round(Math.min(width, height) * glow.radiusRatio)));
  const count = width * height;
  const mask = new Float32Array(count);
  const horizontal = new Float32Array(count);
  for (let i = 0; i < count; i++) mask[i] = highlightMask(data, i * 4, glow.threshold);
  const divisor = radius * 2 + 1;
  for (let y = 0; y < height; y++) {
    const row = y * width;
    let sum = 0;
    for (let offset = -radius; offset <= radius; offset++) sum += mask[row + clamp(offset, 0, width - 1)];
    for (let x = 0; x < width; x++) {
      horizontal[row + x] = sum / divisor;
      sum += mask[row + Math.min(width - 1, x + radius + 1)] - mask[row + Math.max(0, x - radius)];
    }
  }
  // Reuse mask as the final buffer: all subsequent reads are from horizontal.
  for (let x = 0; x < width; x++) {
    let sum = 0;
    for (let offset = -radius; offset <= radius; offset++) sum += horizontal[clamp(offset, 0, height - 1) * width + x];
    for (let y = 0; y < height; y++) {
      mask[y * width + x] = sum / divisor;
      sum += horizontal[Math.min(height - 1, y + radius + 1) * width + x] - horizontal[Math.max(0, y - radius) * width + x];
    }
  }
  return mask;
}

/**
 * Apply an optional Presence look to straight-alpha, encoded-sRGB RGBA bytes.
 * Returns a new buffer; source dimensions, alpha and fully transparent RGB are preserved.
 * No face detection, geometry edit, skin smoothing or selective skin lightening is performed.
 */
export function applyLookToRgba({ data, width, height }, { presetId = 'none', intensity = 1, seed = 1 } = {}) {
  const preset = getPreset(presetId);
  validateIntensity(intensity);
  if (!Number.isSafeInteger(seed)) throw new RangeError('seed must be a safe integer');
  if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height) || width < 1 || height < 1 || !Number.isSafeInteger(width * height * 4)) {
    throw new RangeError('width and height must be positive safe integers');
  }
  if (!(data instanceof Uint8Array || data instanceof Uint8ClampedArray) || data.length !== width * height * 4) {
    throw new RangeError('data must be an RGBA byte array matching width × height × 4');
  }
  const output = new Uint8ClampedArray(data);
  if (preset.id === 'none' || intensity === 0) return output;
  const random = randomSource(seed);
  const glow = preset.glow.strength > 0 ? blurredHighlights(data, width, height, preset.glow) : null;
  const table = toneTable(preset);
  const color = [0, 0, 0];
  for (let y = 0; y < height; y++) {
    const ny = height > 1 ? (2 * y) / (height - 1) - 1 : 0;
    for (let x = 0; x < width; x++) {
      const pixel = y * width + x;
      const offset = pixel * 4;
      const noise = random() + random() - 1;
      if (data[offset + 3] === 0) continue;
      colorTransform(data[offset], data[offset + 1], data[offset + 2], preset, table, color);
      const luminance = (data[offset] * LUMA[0] + data[offset + 1] * LUMA[1] + data[offset + 2] * LUMA[2]) / 255;
      const nx = width > 1 ? (2 * x) / (width - 1) - 1 : 0;
      const radiusSquared = (nx * nx + ny * ny) / 2;
      const vignette = 1 - preset.vignette * radiusSquared * radiusSquared;
      const grain = noise * preset.grain * (0.7 + 0.3 * (1 - Math.abs(2 * luminance - 1)));
      // Only the halo around a highlight is added; the bright source itself is not lifted.
      const halo = glow ? Math.max(0, glow[pixel] - highlightMask(data, offset, preset.glow.threshold)) * preset.glow.strength * 255 : 0;
      for (let channel = 0; channel < 3; channel++) {
        const styled = color[channel] * vignette + grain + halo * preset.glow.tint[channel];
        output[offset + channel] = mix(data[offset + channel], clamp(styled, 0, 255), intensity);
      }
    }
  }
  return output;
}
