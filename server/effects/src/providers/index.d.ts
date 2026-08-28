/// <reference types="node" />
export type ImageMime = 'image/jpeg' | 'image/png' | 'image/webp';
export interface ImageInput { images: Array<{ bytes: Uint8Array; mimeType: ImageMime }>; prompt: string; signal?: AbortSignal }
export interface ImageResult { bytes: Buffer; mimeType: ImageMime; model: string; requestId?: string }
export interface ImageProvider { readonly name: string; readonly model: string; generate(input: ImageInput): Promise<ImageResult> }
export interface ProviderOptions { apiKey: string; model?: string; timeoutMs?: number; fetchImpl?: typeof fetch }
export class ImageProviderError extends Error { readonly code: string; readonly retryable: boolean; readonly requestId?: string; constructor(code: string, options?: {retryable?: boolean; requestId?: string}) }
export class QwenImageProvider implements ImageProvider { readonly name: 'qwen'; readonly model: string; constructor(options: ProviderOptions & {endpoint?: string}); generate(input: ImageInput): Promise<ImageResult> }
export class OpenAIImageProvider implements ImageProvider { readonly name: 'openai'; readonly model: string; constructor(options: ProviderOptions & {quality?: 'low' | 'medium' | 'high'}); generate(input: ImageInput): Promise<ImageResult> }
export const PROMPT_VERSION: 'presence-together-v1';
export function buildTogetherPrompt(options?: {scene?: 'window' | 'walk' | 'cafe'}): string;
export function createImageProviderFromEnv(env?: Record<string, string | undefined>): ImageProvider;
