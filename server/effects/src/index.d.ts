/// <reference types="node" />
export type PresetId = 'none' | 'warm' | 'bw' | 'film' | 'vivid';
export interface RenderOptions { presetId?: PresetId; intensity?: number; seed?: number; webMaxEdge?: number; aiGenerated?: boolean }
export interface RenderMetadata {
  presetId: PresetId; presetVersion: string; intensity: number; seed: number; sourceMimeType: string;
  sourceWidth: number; sourceHeight: number; width: number; height: number; deviceWidth: number; deviceHeight: number;
  deviceQuality: number; sha256: string; bytes: { original: number; web: number; device: number }; warnings: string[];
}
export interface RenderResult { original: Buffer; web: Buffer; device: Buffer; metadata: RenderMetadata }
export class EffectsError extends Error { readonly code: string; constructor(code: string, message: string) }
export const LIMITS: Readonly<{ inputBytes: number; inputPixels: number; webEdge: number; deviceBytes: number }>;
export function detectMime(bytes: Uint8Array): 'image/jpeg' | 'image/png' | 'image/webp';
export function renderPhoto(input: Uint8Array, options?: RenderOptions): Promise<RenderResult>;
export function prepareReference(input: Uint8Array): Promise<{ bytes: Buffer; mimeType: 'image/jpeg'; width: number; height: number; warnings: string[] }>;
export function createPhotoPair(inputs: Uint8Array[], options?: RenderOptions): Promise<RenderResult & {kind: 'photo-pair'; aiGenerated: false}>;
export function rgb565ToRgba(bytes: Uint8Array, width: number, height: number, byteOrder: 'le' | 'be'): Uint8ClampedArray;
export { applyLookToRgba, PRESET_VERSION } from './pixels.js';
export { PRESETS, getPreset } from './presets.js';
