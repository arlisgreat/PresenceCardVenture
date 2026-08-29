import { detectMime, LIMITS } from '../image.js';

export class ImageProviderError extends Error {
  constructor(code, { retryable = false, requestId } = {}) {
    super(code); this.name = 'ImageProviderError'; this.code = code; this.retryable = retryable;
    if (requestId && /^[A-Za-z0-9._:-]{1,160}$/.test(requestId)) this.requestId = requestId;
  }
}

export function validateConfig(apiKey, timeoutMs) {
  if (typeof apiKey !== 'string' || !apiKey.trim()) throw new ImageProviderError('PROVIDER_NOT_CONFIGURED');
  if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 300_000) throw new ImageProviderError('INVALID_PROVIDER_CONFIG');
}

export function validateInput(input, maxImages) {
  if (!input || !Array.isArray(input.images) || input.images.length < 1 || input.images.length > maxImages) throw new ImageProviderError('INVALID_REFERENCES');
  if (typeof input.prompt !== 'string' || !input.prompt.trim() || input.prompt.length > 12_000) throw new ImageProviderError('INVALID_PROMPT');
  let total = 0;
  for (const image of input.images) {
    if (!(image.bytes instanceof Uint8Array) || !image.bytes.length || image.bytes.length > LIMITS.inputBytes) throw new ImageProviderError('INVALID_REFERENCES');
    try { if (detectMime(image.bytes) !== image.mimeType) throw new Error('mime'); }
    catch { throw new ImageProviderError('INVALID_REFERENCES'); }
    total += image.bytes.length;
  }
  if (total > 20 * 1024 * 1024) throw new ImageProviderError('INVALID_REFERENCES');
}

export async function withDeadline(signal, timeoutMs, fn) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  const combined = signal ? AbortSignal.any([signal, controller.signal]) : controller.signal;
  try {
    combined.throwIfAborted();
    return await fn(combined);
  } catch (error) {
    if (signal?.aborted) throw new ImageProviderError('PROVIDER_CANCELLED');
    if (controller.signal.aborted) throw new ImageProviderError('PROVIDER_TIMEOUT');
    if (error instanceof ImageProviderError) throw error;
    // Never expose credentials, provider response bodies, signed URLs or prompts.
    // A disconnected POST may already have been charged. Never automatically resubmit it.
    throw new ImageProviderError('PROVIDER_UNAVAILABLE');
  } finally { clearTimeout(timer); }
}

export async function readLimited(response, maxBytes) {
  const claimedLength = Number(response.headers.get('content-length') ?? 0);
  if (claimedLength > maxBytes) {
    await response.body?.cancel().catch(() => {});
    throw new ImageProviderError('INVALID_PROVIDER_OUTPUT');
  }
  if (!response.body) throw new ImageProviderError('INVALID_PROVIDER_OUTPUT');
  const reader = response.body.getReader(), chunks = [];
  let total = 0;
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      total += value.byteLength;
      if (total > maxBytes) { await reader.cancel(); throw new ImageProviderError('INVALID_PROVIDER_OUTPUT'); }
      chunks.push(Buffer.from(value));
    }
  } finally { reader.releaseLock(); }
  return Buffer.concat(chunks, total);
}

export async function providerJson(fetchImpl, url, init) {
  const response = await fetchImpl(url, { ...init, redirect: 'error' });
  const requestId = response.headers.get('x-request-id') ?? undefined;
  if (!response.ok) {
    await response.body?.cancel().catch(() => {});
    const status = response.status;
    const code = status === 401 || status === 403 ? 'PROVIDER_AUTH' : status === 429 ? 'PROVIDER_RATE_LIMITED'
      : status >= 500 ? 'PROVIDER_UNAVAILABLE' : 'PROVIDER_REJECTED';
    throw new ImageProviderError(code, { retryable: status === 429, requestId });
  }
  const bytes = await readLimited(response, 24 * 1024 * 1024);
  try { return { json: JSON.parse(bytes.toString('utf8')), requestId }; }
  catch { throw new ImageProviderError('INVALID_PROVIDER_OUTPUT', { requestId }); }
}

export function validatedOutput(bytes) {
  if (!bytes.length || bytes.length > LIMITS.inputBytes) throw new ImageProviderError('INVALID_PROVIDER_OUTPUT');
  try { return { bytes, mimeType: detectMime(bytes) }; }
  catch { throw new ImageProviderError('INVALID_PROVIDER_OUTPUT'); }
}
