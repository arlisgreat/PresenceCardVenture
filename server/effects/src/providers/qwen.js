import { ImageProviderError, validateConfig, validateInput, withDeadline, providerJson, readLimited, validatedOutput } from './common.js';

const DEFAULT_ENDPOINT = 'https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation';
function endpointFor(value) {
  let url;
  try { url = new URL(value); } catch { throw new ImageProviderError('INVALID_PROVIDER_CONFIG'); }
  const workspace = /^[a-z0-9-]+\.(cn-beijing|ap-southeast-1|eu-central-1|ap-northeast-1)\.maas\.aliyuncs\.com$/;
  if (url.protocol !== 'https:' || url.username || url.password || url.port || url.search || url.hash ||
      !(url.hostname === 'dashscope.aliyuncs.com' || workspace.test(url.hostname)) ||
      url.pathname !== '/api/v1/services/aigc/multimodal-generation/generation') throw new ImageProviderError('INVALID_PROVIDER_CONFIG');
  return url.href;
}

export function validateQwenOutputUrl(value) {
  let url;
  try { url = new URL(value); } catch { throw new ImageProviderError('INVALID_PROVIDER_OUTPUT'); }
  // Only vendor result buckets, never arbitrary user URLs, redirects or local services.
  if (url.protocol !== 'https:' || url.username || url.password || url.port || url.hash ||
      !/^dashscope-result-[a-z0-9-]+\.oss-[a-z0-9-]+\.aliyuncs\.com$/.test(url.hostname)) throw new ImageProviderError('INVALID_PROVIDER_OUTPUT');
  return url.href;
}

export class QwenImageProvider {
  name = 'qwen';
  #apiKey; #endpoint; #fetch; #timeoutMs;
  constructor({ apiKey, model = 'qwen-image-3.0-pro', endpoint = DEFAULT_ENDPOINT, timeoutMs = 180_000, fetchImpl = fetch } = {}) {
    validateConfig(apiKey, timeoutMs);
    if (!['qwen-image-3.0-pro', 'qwen-image-3.0'].includes(model)) throw new ImageProviderError('INVALID_PROVIDER_CONFIG');
    this.model = model; this.#apiKey = apiKey.trim(); this.#endpoint = endpointFor(endpoint); this.#timeoutMs = timeoutMs; this.#fetch = fetchImpl;
  }
  async generate(input) {
    validateInput(input, 3);
    return withDeadline(input.signal, this.#timeoutMs, async signal => {
      const { json } = await providerJson(this.#fetch, this.#endpoint, {
        method: 'POST', signal,
        headers: { Authorization: `Bearer ${this.#apiKey}`, 'Content-Type': 'application/json' },
        body: JSON.stringify({
          model: this.model,
          input: { messages: [{ role: 'user', content: [
            ...input.images.map(image => ({ image: `data:${image.mimeType};base64,${Buffer.from(image.bytes).toString('base64')}` })),
            { text: input.prompt },
          ] }] },
          parameters: { n: 1, size: '1024*1024', prompt_extend: false, watermark: true },
        }),
      });
      if (json.code) {
        const code = /api.?key|auth|accessdenied/i.test(json.code) ? 'PROVIDER_AUTH'
          : /throttl|limit|rate/i.test(json.code) ? 'PROVIDER_RATE_LIMITED' : 'PROVIDER_REJECTED';
        throw new ImageProviderError(code, { requestId: json.request_id, retryable: code === 'PROVIDER_RATE_LIMITED' });
      }
      const images = json.output?.choices?.flatMap(choice => choice.message?.content ?? []).filter(item => typeof item.image === 'string');
      if (!images || images.length !== 1) throw new ImageProviderError('INVALID_PROVIDER_OUTPUT');
      const url = validateQwenOutputUrl(images[0].image);
      // Deliberately no Authorization header on the signed object-storage download.
      const response = await this.#fetch(url, { method: 'GET', signal, redirect: 'error' });
      if (!response.ok) { await response.body?.cancel().catch(() => {}); throw new ImageProviderError('PROVIDER_DOWNLOAD_FAILED'); }
      const output = validatedOutput(await readLimited(response, 10 * 1024 * 1024));
      return { ...output, model: this.model, requestId: typeof json.request_id === 'string' && /^[A-Za-z0-9._:-]{1,160}$/.test(json.request_id) ? json.request_id : undefined };
    });
  }
}
