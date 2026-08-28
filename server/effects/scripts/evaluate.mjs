import { parseArgs } from 'node:util';
import { performance } from 'node:perf_hooks';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import sharp from 'sharp';
import { PRESETS, PRESET_VERSION, renderPhoto } from '../src/index.js';
const { values } = parseArgs({ options: { input: { type: 'string' }, out: { type: 'string' }, repeats: { type: 'string', default: '10' } } });
if (!values.input || !values.out) throw new Error('Usage: npm run evaluate -- --input examples/source.png --out output/benchmark.json');
const repeats = Number(values.repeats);
if (!Number.isInteger(repeats) || repeats < 3 || repeats > 100) throw new Error('repeats must be 3..100');
const source = await readFile(values.input);
const variants = [
  { id: 'web', input: source },
  { id: 'qvga-resized-fixture', input: await sharp(source).resize(320, 240, { fit: 'contain' }).jpeg().toBuffer() },
];
const rows = [];
const quantile = (values, q) => [...values].sort((a, b) => a - b)[Math.ceil(values.length * q) - 1];
for (const variant of variants) for (const preset of PRESETS) {
  await renderPhoto(variant.input, { presetId: preset.id, seed: 72 });
  const times = []; let result;
  for (let i = 0; i < repeats; i++) {
    const start = performance.now();
    result = await renderPhoto(variant.input, { presetId: preset.id, seed: 72 });
    times.push(performance.now() - start);
  }
  rows.push({ source: variant.id, preset: preset.id, n: repeats, p50Ms: +quantile(times, .5).toFixed(2), p95Ms: +quantile(times, .95).toFixed(2), maxMs: +Math.max(...times).toFixed(2), webBytes: result.web.length, deviceBytes: result.device.length, width: result.metadata.width, height: result.metadata.height });
}
const report = { kind: 'local-filter-benchmark', date: new Date().toISOString(), node: process.version, platform: `${process.platform}/${process.arch}`, cpu: os.cpus()[0]?.model, presetVersion: PRESET_VERSION, scope: 'sequential local decode + filter + Web/device encoding; synthetic fixture; no model API, network, queue or real CoreS3 capture', rows };
await mkdir(path.dirname(path.resolve(values.out)), { recursive: true });
await writeFile(values.out, JSON.stringify(report, null, 2) + '\n', { flag: 'wx', mode: 0o600 });
console.log(JSON.stringify(report));
