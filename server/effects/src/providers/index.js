export { ImageProviderError } from './common.js';
export { QwenImageProvider } from './qwen.js';
export { OpenAIImageProvider } from './openai.js';
export { buildTogetherPrompt, PROMPT_VERSION } from './prompt.js';

import { ImageProviderError } from './common.js';
import { QwenImageProvider } from './qwen.js';
import { OpenAIImageProvider } from './openai.js';

/** Explicit selection only: never transfer private photos to a fallback vendor. */
export function createImageProviderFromEnv(env = process.env) {
  const provider = String(env.AI_PROVIDER ?? '').trim().toLowerCase();
  const timeoutMs = env.IMAGE_TIMEOUT_MS === undefined ? 180_000 : Number(env.IMAGE_TIMEOUT_MS);
  if (provider === 'qwen') return new QwenImageProvider({ apiKey: env.DASHSCOPE_API_KEY, model: env.IMAGE_MODEL || undefined, endpoint: env.QWEN_IMAGE_ENDPOINT || undefined, timeoutMs });
  if (provider === 'openai') return new OpenAIImageProvider({ apiKey: env.OPENAI_API_KEY, model: env.IMAGE_MODEL || undefined, quality: env.OPENAI_IMAGE_QUALITY || 'medium', timeoutMs });
  throw new ImageProviderError('PROVIDER_NOT_CONFIGURED');
}
