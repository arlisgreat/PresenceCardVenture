import type { PresetId, Rgb } from './presets.js';
export { PRESET_VERSION, PRESETS, getPreset } from './presets.js';
export type { PresetId, Preset, Rgb } from './presets.js';
export interface LookOptions { presetId?: PresetId; intensity?: number; seed?: number }
export interface RgbaImage { data: Uint8Array | Uint8ClampedArray; width: number; height: number }
/** New unquantized 0–255 RGB tuple. Spatial effects are excluded. */
export function transformRgb(rgb: Rgb, presetId?: PresetId, intensity?: number): [number, number, number];
/** New same-size RGBA buffer; alpha and fully transparent RGB are unchanged. */
export function applyLookToRgba(image: RgbaImage, options?: LookOptions): Uint8ClampedArray;
