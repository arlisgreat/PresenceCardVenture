import { ImageProviderError, validateConfig, validateInput, withDeadline, providerJson, validatedOutput } from './common.js';

export class OpenAIImageProvider {
  name = 'openai';
  #apiKey; #fetch; #timeoutMs; #quality;
  constructor({ apiKey, model = 'gpt-image-2-2026-04-21', quality = 'medium', timeoutMs = 180_000, fetchImpl = fetch } = {}) {
    validateConfig(apiKey, timeoutMs);
    if (!['gpt-image-2', 'gpt-image-2-2026-04-21'].includes(model) || !['low', 'medium', 'high'].includes(quality)) throw new ImageProviderError('INVALID_PROVIDER_CONFIG');
    this.model = model; this.#apiKey = apiKey.trim(); this.#fetch = fetchImpl; this.#timeoutMs = timeoutMs; this.#quality = quality;
  }
  async generate(input) {
    validateInput(input, 16);
    return withDeadline(input.signal, this.#timeoutMs, async signal => {
      const body = new FormData();
      body.set('model', this.model); body.set('prompt', input.prompt); body.set('n', '1');
      body.set('size', '1024x1024'); body.set('quality', this.#quality); body.set('output_format', 'jpeg'); body.set('output_compression', '92');
      // GPT Image 2 always uses high input fidelity; input_fidelity is not accepted.
      input.images.forEach((image, i) => {
        const extension = image.mimeType === 'image/jpeg' ? 'jpg' : image.mimeType.split('/')[1];
        body.append('image[]', new Blob([image.bytes], { type: image.mimeType }), `reference-${i + 1}.${extension}`);
      });
      const { json, requestId } = await providerJson(this.#fetch, 'https://api.openai.com/v1/images/edits', {
        method: 'POST', signal, headers: { Authorization: `Bearer ${this.#apiKey}` }, body,
      });
      const encoded = json.data?.[0]?.b64_json;
      if (json.data?.length !== 1 || typeof encoded !== 'string' || !encoded.length || encoded.length > 14 * 1024 * 1024 ||
          encoded.length % 4 !== 0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(encoded)) throw new ImageProviderError('INVALID_PROVIDER_OUTPUT');
      return { ...validatedOutput(Buffer.from(encoded, 'base64')), model: this.model, requestId };
    });
  }
}
