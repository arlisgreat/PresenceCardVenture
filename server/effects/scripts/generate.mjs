import { parseArgs } from 'node:util';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { prepareReference, renderPhoto } from '../src/index.js';
import { createImageProviderFromEnv, buildTogetherPrompt, PROMPT_VERSION } from '../src/providers/index.js';

// Explicitly invoked operator smoke test. It never reads user-gallery files automatically.
const { values } = parseArgs({ options: { first: { type: 'string' }, second: { type: 'string' }, authorization: { type: 'string' }, out: { type: 'string' }, scene: { type: 'string', default: 'window' }, execute: { type: 'boolean', default: false } } });
if (!values.first || !values.second || !values.out) throw new Error('Usage: npm run generate -- --first a.jpg --second b.jpg --out output/ai-run [--authorization private-grants.json --execute]');
const files = [await readFile(values.first), await readFile(values.second)];
const hashes = files.map(bytes => createHash('sha256').update(bytes).digest('hex'));
if (new Set(hashes).size !== 2) throw new Error('Choose two different reference images');
const references = await Promise.all(files.map(prepareReference));
if (references.some(reference => Math.min(reference.width, reference.height) < 128)) throw new Error('Reference too small; provide a clearer original');
const prompt = buildTogetherPrompt({ scene: values.scene });
if (!values.execute) {
  console.log(JSON.stringify({ dryRun: true, provider: process.env.AI_PROVIDER || 'not-configured', hashes, warnings: references.map(reference => reference.warnings), promptVersion: PROMPT_VERSION, prompt, note: 'No upload or model call. --execute requires a server-owned, owner-approved authorization manifest.' }, null, 2));
} else {
  if (!values.authorization) throw new Error('Authorization manifest required');
  const provider = createImageProviderFromEnv();
  const grants = JSON.parse(await readFile(values.authorization, 'utf8'));
  const authorized = hashes.every(hash => grants.materials?.some(material => material.sha256 === hash && typeof material.ownerId === 'string' && material.ownerId && material.approved === true && material.provider === provider.name && material.model === provider.model && material.purpose === 'generate' && Date.parse(material.expiresAt) > Date.now()));
  if (!authorized) throw new Error('Missing current owner authorization for this provider, model and purpose');
  const out = path.resolve(values.out);
  // Reserve a new output directory BEFORE billing; never overwrite a prior/private result.
  await mkdir(path.dirname(out), { recursive: true });
  await mkdir(out, { recursive: false, mode: 0o700 });
  const started = performance.now();
  try {
    const generated = await provider.generate({ images: references.map(({ bytes, mimeType }) => ({ bytes, mimeType })), prompt });
    const rendered = await renderPhoto(generated.bytes, { presetId: 'warm', intensity: .6, seed: 72, aiGenerated: true });
    const extension = generated.mimeType.split('/')[1] === 'jpeg' ? 'jpg' : generated.mimeType.split('/')[1];
    const metadata = { aiGenerated: true, provider: provider.name, model: generated.model, requestId: generated.requestId, promptVersion: PROMPT_VERSION, prompt, referenceHashes: hashes, elapsedMs: +(performance.now() - started).toFixed(2), measuredCost: null, visualAcceptance: 'pending', output: rendered.metadata };
    await Promise.all([
      writeFile(path.join(out, `source.${extension}`), rendered.original, { flag: 'wx', mode: 0o600 }),
      writeFile(path.join(out, 'web.jpg'), rendered.web, { flag: 'wx', mode: 0o600 }),
      writeFile(path.join(out, 'device.jpg'), rendered.device, { flag: 'wx', mode: 0o600 }),
      writeFile(path.join(out, 'AI-GENERATED.json'), JSON.stringify(metadata, null, 2) + '\n', { flag: 'wx', mode: 0o600 }),
    ]);
    console.log(JSON.stringify({ output: out, aiGenerated: true, provider: provider.name, model: provider.model, visualAcceptance: 'pending' }));
  } catch (error) {
    const code = typeof error?.code === 'string' && /^[A-Z_]+$/.test(error.code) ? error.code : 'GENERATION_FAILED';
    await writeFile(path.join(out, 'failure.json'), JSON.stringify({ code, elapsedMs: +(performance.now() - started).toFixed(2), billingMayHaveOccurred: true, automaticRetry: false }) + '\n', { flag: 'wx', mode: 0o600 });
    console.error(code); process.exitCode = 1;
  }
}
