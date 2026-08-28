export type PresetId = 'none' | 'warm' | 'bw' | 'film' | 'vivid';
export type Rgb = readonly [number, number, number];
export interface Preset {
  readonly id: PresetId;
  readonly name: string;
  readonly description: string;
  readonly accent: string;
  readonly version: string;
  readonly saturation: number;
  readonly toneCurve: readonly (readonly [number, number])[];
  readonly channelGain: Rgb;
  readonly shadowTint: Rgb;
  readonly highlightTint: Rgb;
  /** Maximum absolute grain amplitude, in 8-bit channel levels. */
  readonly grain: number;
  readonly vignette: number;
  readonly glow: Readonly<{ strength: number; threshold: number; tint: Rgb; radiusRatio: number }>;
}
export const PRESET_VERSION: string;
export const PRESETS: readonly Preset[];
export function getPreset(id: PresetId | string): Preset;
