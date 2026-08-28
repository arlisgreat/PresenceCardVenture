import { parseArgs } from 'node:util';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import sharp from 'sharp';
import { renderPhoto, PRESETS, PRESET_VERSION } from '../src/index.js';

const { values } = parseArgs({ options: { input: { type: 'string' }, out: { type: 'string' }, preset: { type: 'string', default: 'warm' }, all: { type: 'boolean', default: false }, seed: { type: 'string', default: '20260827' }, intensity: { type: 'string', default: '1' } } });
if (!values.input || !values.out) throw new Error('Usage: npm run render -- --input image.jpg --out output/new-run --all');
const input = await readFile(values.input), out = path.resolve(values.out);
await mkdir(out, { recursive: true });
const write = (name, data) => writeFile(path.join(out, name), data, { flag: 'wx', mode: 0o600 });
const selected = values.all ? PRESETS : PRESETS.filter(preset => preset.id === values.preset);
if (!selected.length) throw new Error('Unknown preset');
const entries = [], panels = [];
const label = (text, detail) => Buffer.from(`<svg width="480" height="68" xmlns="http://www.w3.org/2000/svg"><rect width="480" height="68" fill="#fbf9f7"/><text x="16" y="29" fill="#3a3f45" font-family="sans-serif" font-size="19">${text}</text><text x="16" y="52" fill="#73797e" font-family="sans-serif" font-size="12">${detail}</text></svg>`);
for (const preset of selected) {
  const result = await renderPhoto(input, { presetId: preset.id, seed: Number(values.seed), intensity: Number(values.intensity) });
  await write(`${preset.id}.jpg`, result.web);
  await write(`${preset.id}-device.jpg`, result.device);
  entries.push({ name: preset.name, webFile: `${preset.id}.jpg`, deviceFile: `${preset.id}-device.jpg`, ...result.metadata });
  panels.push({ image: await sharp(result.web).resize(480, 360, { fit: 'contain', background: '#fbf9f7' }).toBuffer(), label: label(preset.id.toUpperCase(), `${preset.name} / ${PRESET_VERSION}`) });
}
if (values.all) {
  const device = await readFile(path.join(out, 'film-device.jpg'));
  const devicePanel = await sharp({ create: { width: 480, height: 360, channels: 3, background: '#e9e2dd' } }).composite([{ input: device, left: 80, top: 60 }]).png().toBuffer();
  panels.push({ image: devicePanel, label: label('DEVICE / 320 x 240', 'Baseline JPEG - no face crop - original pixel size') });
}
const cols = Math.min(3, panels.length), rows = Math.ceil(panels.length / cols), gap = 24;
const canvasWidth = cols * 480 + (cols + 1) * gap, canvasHeight = rows * 428 + (rows + 1) * gap + 108;
const composites = [{ input: Buffer.from(`<svg width="${canvasWidth}" height="108" xmlns="http://www.w3.org/2000/svg"><rect width="100%" height="100%" fill="#fbf9f7"/><text x="24" y="45" fill="#3a3f45" font-family="sans-serif" font-size="26">PRESENCE / ORIGINAL LOOKS</text><text x="24" y="77" fill="#73797e" font-family="sans-serif" font-size="14">ONE SOURCE - DETERMINISTIC COLOR - NOT AN AI CO-PHOTO BENCHMARK</text></svg>`), left: 0, top: 0 }];
panels.forEach((panel, i) => {
  const left = gap + (i % cols) * (480 + gap), top = 108 + gap + Math.floor(i / cols) * (428 + gap);
  composites.push({ input: panel.image, left, top }, { input: panel.label, left, top: top + 360 });
});
const sheet = await sharp({ create: { width: canvasWidth, height: canvasHeight, channels: 3, background: '#fbf9f7' } }).composite(composites).png().toBuffer();
await write('comparison.png', sheet);
await write('manifest.json', JSON.stringify({ kind: 'deterministic-filter-comparison', sourceFile: path.basename(values.input), presetVersion: PRESET_VERSION, entries }, null, 2) + '\n');
console.log(JSON.stringify({ output: out, presets: selected.map(preset => preset.id), comparison: path.join(out, 'comparison.png') }));
