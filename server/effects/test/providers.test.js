import test from 'node:test';
import assert from 'node:assert/strict';
import sharp from 'sharp';
import { QwenImageProvider, OpenAIImageProvider, buildTogetherPrompt, createImageProviderFromEnv } from '../src/providers/index.js';

const photo = await sharp({ create: { width: 32, height: 32, channels: 3, background: '#baaa99' } }).jpeg().toBuffer();
const input = { images: [{ bytes: photo, mimeType: 'image/jpeg' }, { bytes: photo, mimeType: 'image/jpeg' }], prompt: buildTogetherPrompt() };
const jsonResponse = (data, status = 200) => new Response(JSON.stringify(data), { status, headers: { 'content-type': 'application/json', 'x-request-id': 'test-request' } });
const qwenResponse = (url = 'https://dashscope-result-bj.oss-cn-beijing.aliyuncs.com/image.png?Expires=1') => jsonResponse({ request_id: 'qwen-test', output: { choices: [{ message: { content: [{ image: url }] } }] } });

test('Qwen uses 3.0 API, two references, authored prompt and watermark; no download credentials', async () => {
  const requests = [];
  const provider = new QwenImageProvider({ apiKey: 'test-secret', fetchImpl: async (url, init) => {
    requests.push({ url, init }); return requests.length === 1 ? qwenResponse() : new Response(photo);
  } });
  const result = await provider.generate(input);
  const payload = JSON.parse(requests[0].init.body);
  assert.equal(payload.model, 'qwen-image-3.0-pro');
  assert.equal(payload.input.messages[0].content.length, 3);
  assert.equal(payload.parameters.prompt_extend, false); assert.equal(payload.parameters.watermark, true);
  assert.equal(payload.parameters.n, 1); assert.equal(payload.parameters.size, '1024*1024');
  assert.equal(requests[0].init.redirect, 'error'); assert.equal(requests[1].init.redirect, 'error');
  assert.equal(requests[1].init.headers, undefined); assert.deepEqual(result.bytes, photo);
  assert.equal(result.requestId, 'qwen-test');
});

test('GPT Image 2 sends multipart image[]; omits unsupported input_fidelity', async () => {
  let request;
  const provider = new OpenAIImageProvider({ apiKey: 'test-secret', fetchImpl: async (url, init) => {
    request = { url, init }; return jsonResponse({ data: [{ b64_json: photo.toString('base64') }] });
  } });
  const result = await provider.generate(input);
  assert.equal(request.url, 'https://api.openai.com/v1/images/edits');
  assert.equal(request.init.body.get('model'), 'gpt-image-2-2026-04-21');
  assert.equal(request.init.body.getAll('image[]').length, 2);
  assert.equal(request.init.body.has('input_fidelity'), false);
  assert.equal(request.init.body.has('response_format'), false);
  assert.equal(request.init.headers['Content-Type'], undefined, 'fetch owns multipart boundary');
  assert.equal(request.init.body.get('quality'), 'medium'); assert.deepEqual(result.bytes, photo);
});

test('rejects untrusted endpoints, output URLs and content without a second request', async () => {
  for (const endpoint of ['http://dashscope.aliyuncs.com/', 'https://example.com/api', 'https://dashscope.aliyuncs.com.evil.test/api', 'https://u:p@dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation']) {
    assert.throws(() => new QwenImageProvider({ apiKey: 'test', endpoint }), { code: 'INVALID_PROVIDER_CONFIG' });
  }
  for (const url of ['http://127.0.0.1/', 'https://example.com/photo.jpg', 'https://dashscope-result-bj.oss-cn-beijing.aliyuncs.com.evil.test/x', 'https://user:pass@dashscope-result-bj.oss-cn-beijing.aliyuncs.com/x']) {
    let calls = 0;
    const provider = new QwenImageProvider({ apiKey: 'test', fetchImpl: async () => { calls++; return qwenResponse(url); } });
    await assert.rejects(provider.generate(input), { code: 'INVALID_PROVIDER_OUTPUT' }); assert.equal(calls, 1);
  }
  const malformed = new OpenAIImageProvider({ apiKey: 'test', fetchImpl: async () => jsonResponse({ data: [{ b64_json: '<html>' }] }) });
  await assert.rejects(malformed.generate(input), { code: 'INVALID_PROVIDER_OUTPUT' });
});

test('never retries a billed generation; safe failures do not leak response bodies or secrets', async () => {
  for (const [status, code] of [[401, 'PROVIDER_AUTH'], [403, 'PROVIDER_AUTH'], [400, 'PROVIDER_REJECTED'], [429, 'PROVIDER_RATE_LIMITED'], [500, 'PROVIDER_UNAVAILABLE']]) {
    let calls = 0;
    const provider = new OpenAIImageProvider({ apiKey: 'test-secret', fetchImpl: async () => { calls++; return jsonResponse({ error: 'test-secret private prompt' }, status); } });
    await assert.rejects(provider.generate(input), error => {
      assert.equal(error.code, code); assert.equal(error.retryable, status === 429);
      assert.equal(error.message.includes('test-secret'), false); assert.equal(error.message.includes('private prompt'), false); return true;
    });
    assert.equal(calls, 1);
  }
  const dropped = new OpenAIImageProvider({ apiKey: 'test', fetchImpl: async () => { throw new TypeError('billed POST disconnected: test-secret'); } });
  await assert.rejects(dropped.generate(input), error => error.code === 'PROVIDER_UNAVAILABLE' && error.retryable === false);
});

test('timeout and cancellation abort outstanding requests without leaking or retrying', async () => {
  const blockedFetch = async (_url, init) => new Promise((_resolve, reject) => {
    if (init.signal.aborted) reject(new Error('aborted'));
    else init.signal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
  });
  const provider = new OpenAIImageProvider({ apiKey: 'test', timeoutMs: 20, fetchImpl: blockedFetch });
  await assert.rejects(provider.generate(input), { code: 'PROVIDER_TIMEOUT' });
  const controller = new AbortController(); controller.abort();
  await assert.rejects(provider.generate({ ...input, signal: controller.signal }), { code: 'PROVIDER_CANCELLED' });
});

test('validates reference bytes/MIME/count before spending', async () => {
  let calls = 0;
  const provider = new QwenImageProvider({ apiKey: 'test', fetchImpl: async () => { calls++; return qwenResponse(); } });
  await assert.rejects(provider.generate({ ...input, images: [] }), { code: 'INVALID_REFERENCES' });
  await assert.rejects(provider.generate({ ...input, images: [{ bytes: photo, mimeType: 'image/png' }] }), { code: 'INVALID_REFERENCES' });
  await assert.rejects(provider.generate({ ...input, prompt: '' }), { code: 'INVALID_PROMPT' });
  assert.equal(calls, 0);
});

test('enforces bounded downloads even when Content-Length is missing or misleading', async () => {
  for (const header of [undefined, '20000000']) {
    let calls = 0;
    const provider = new QwenImageProvider({ apiKey: 'test', fetchImpl: async () => {
      calls++; return calls === 1 ? qwenResponse() : new Response(Buffer.alloc(10 * 1024 * 1024 + 1), { headers: header ? { 'content-length': header } : {} });
    } });
    await assert.rejects(provider.generate(input), { code: 'INVALID_PROVIDER_OUTPUT' }); assert.equal(calls, 2);
  }
});

test('environment selects one vendor explicitly and never silently falls back', () => {
  assert.throws(() => createImageProviderFromEnv({}), { code: 'PROVIDER_NOT_CONFIGURED' });
  assert.throws(() => createImageProviderFromEnv({ AI_PROVIDER: 'qwen', OPENAI_API_KEY: 'test' }), { code: 'PROVIDER_NOT_CONFIGURED' });
  assert.equal(createImageProviderFromEnv({ AI_PROVIDER: 'openai', OPENAI_API_KEY: 'test' }).name, 'openai');
  assert.equal(createImageProviderFromEnv({ AI_PROVIDER: 'qwen', DASHSCOPE_API_KEY: 'test' }).model, 'qwen-image-3.0-pro');
  assert.throws(() => buildTogetherPrompt({ scene: 'unapproved' }), RangeError);
});
