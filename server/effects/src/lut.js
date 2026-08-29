import { PRESET_VERSION, getPreset } from './presets.js';
import { transformRgb } from './pixels.js';

function validateOptions(presetId, size, intensity) {
  getPreset(presetId);
  if (!Number.isInteger(size) || size < 2 || size > 65) throw new RangeError('LUT size must be an integer between 2 and 65');
  if (!Number.isFinite(intensity) || intensity < 0 || intensity > 1) throw new RangeError('intensity must be a finite number between 0 and 1');
}

/** Red varies fastest: index = (blue * size + green) * size + red. */
function* samples(presetId, size, intensity) {
  for (let blue = 0; blue < size; blue++) {
    for (let green = 0; green < size; green++) {
      for (let red = 0; red < size; red++) {
        yield transformRgb([red * 255 / (size - 1), green * 255 / (size - 1), blue * 255 / (size - 1)], presetId, intensity);
      }
    }
  }
}

/** Standard 3D .cube color LUT. Excludes grain, vignette and spatial highlight glow. */
export function generateCube(presetId, { size = 17, intensity = 1 } = {}) {
  validateOptions(presetId, size, intensity);
  const lines = [
    `TITLE "Presence ${presetId} ${PRESET_VERSION}"`,
    '# Encoded sRGB; red index varies fastest.',
    '# Color only: grain, vignette and spatial highlight glow are not included.',
    `# Intensity ${intensity}`,
    `LUT_3D_SIZE ${size}`, 'DOMAIN_MIN 0.0 0.0 0.0', 'DOMAIN_MAX 1.0 1.0 1.0',
  ];
  for (const rgb of samples(presetId, size, intensity)) lines.push(rgb.map((channel) => (channel / 255).toFixed(7)).join(' '));
  return `${lines.join('\n')}\n`;
}

/** Numeric RGB565 words, not a byte stream. Firmware must choose the display bus byte order. */
export function generateRgb565Lut(presetId, { size = 17, intensity = 1 } = {}) {
  validateOptions(presetId, size, intensity);
  const output = new Uint16Array(size ** 3);
  let index = 0;
  for (const [r, g, b] of samples(presetId, size, intensity)) {
    output[index++] = (Math.round(r * 31 / 255) << 11) | (Math.round(g * 63 / 255) << 5) | Math.round(b * 31 / 255);
  }
  return output;
}

const C_KEYWORDS = new Set('alignas alignof auto bool break case char const constexpr continue default do double else enum extern false float for goto if inline int long nullptr register restrict return short signed sizeof static static_assert struct switch thread_local true typedef typeof typeof_unqual union unsigned void volatile while _Alignas _Alignof _Atomic _BitInt _Bool _Complex _Decimal32 _Decimal64 _Decimal128 _Generic _Imaginary _Noreturn _Static_assert _Thread_local'.split(' '));

/** Self-contained RGB565 header with a heap-free, nearest-neighbor lookup helper. */
export function generateRgb565Header(presetId, { size = 17, intensity = 1, symbol = `presence_${presetId}_rgb565_lut` } = {}) {
  validateOptions(presetId, size, intensity);
  if (typeof symbol !== 'string' || !/^[A-Za-z][A-Za-z0-9_]*$/.test(symbol) || C_KEYWORDS.has(symbol)) {
    throw new RangeError('symbol must be a non-reserved C identifier starting with a letter');
  }
  const lut = generateRgb565Lut(presetId, { size, intensity });
  const lines = [
    '#pragma once', '#include <stdint.h>', '',
    `// Presence ${presetId}; ${PRESET_VERSION}; intensity ${intensity}.`,
    '// Encoded sRGB. Index: (blue * size + green) * size + red; red varies fastest.',
    '// Color only; grain, vignette and spatial highlight glow are excluded.',
    '// Numeric RGB565 words; select byte order for the display bus.',
    '// Lookup uses nearest grid samples, not trilinear interpolation: expect color quantization.',
    `static const uint8_t ${symbol}_size = ${size};`,
    `static const uint16_t ${symbol}[${lut.length}] = {`,
  ];
  for (let i = 0; i < lut.length; i += 12) {
    lines.push(`  ${Array.from(lut.subarray(i, i + 12), (value) => `0x${value.toString(16).padStart(4, '0')}`).join(', ')},`);
  }
  lines.push('};', '');
  lines.push(`static inline uint16_t ${symbol}_apply_rgb565(uint16_t pixel) {`);
  if (presetId === 'none' || intensity === 0) {
    // A coarse identity LUT is not lossless. The no-effect path must bypass lookup.
    lines.push('  // Preserve original RGB565 exactly; bypass the coarse identity grid.', '  return pixel;');
  } else {
    lines.push(
      `  const uint32_t red = ((((uint32_t)pixel >> 11) & 31u) * ${size - 1}u + 15u) / 31u;`,
      `  const uint32_t green = ((((uint32_t)pixel >> 5) & 63u) * ${size - 1}u + 31u) / 63u;`,
      `  const uint32_t blue = (((uint32_t)pixel & 31u) * ${size - 1}u + 15u) / 31u;`,
      `  const uint32_t index = (blue * ${size}u + green) * ${size}u + red;`,
      `  return ${symbol}[index];`,
    );
  }
  lines.push('}', '');
  return lines.join('\n');
}
