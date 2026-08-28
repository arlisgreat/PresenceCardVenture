import { parseArgs } from 'node:util';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { PRESETS, PRESET_VERSION } from '../src/presets.js';
import { generateCube, generateRgb565Header } from '../src/lut.js';
const { values } = parseArgs({ options: { out: { type: 'string' }, size: { type: 'string', default: '17' } } });
if (!values.out) throw new Error('Usage: npm run luts -- --out output/luts');
const out = path.resolve(values.out), size = Number(values.size);
await mkdir(out, { recursive: true });
for (const preset of PRESETS) {
  await writeFile(path.join(out, `${preset.id}.cube`), generateCube(preset.id, { size }), { flag: 'wx' });
  await writeFile(path.join(out, `${preset.id}.h`), generateRgb565Header(preset.id, { size }), { flag: 'wx' });
}
await writeFile(path.join(out, 'manifest.json'), JSON.stringify({ presetVersion: PRESET_VERSION, size, entries: size ** 3, rgb565BytesPerPreset: 2 * size ** 3, order: 'red-fastest', spatialEffects: false, hardwareFps: 'not-tested' }, null, 2) + '\n', { flag: 'wx' });
console.log(JSON.stringify({ out, presets: PRESETS.length, rgb565BytesPerPreset: 2 * size ** 3 }));
