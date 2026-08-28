import { parseArgs } from 'node:util';
import { createRequire } from 'node:module';
import { createHash } from 'node:crypto';
import { mkdir, writeFile, readdir, copyFile, readFile } from 'node:fs/promises';
import path from 'node:path';
import { constants } from 'node:fs';

const { values } = parseArgs({ options: { out: { type: 'string', default: 'assets/live' } } });
const require = createRequire(import.meta.url);
const packagePath = path.join(path.dirname(require.resolve('@mediapipe/tasks-vision')), 'package.json');
const installed = JSON.parse(await readFile(packagePath, 'utf8'));
if (installed.version !== '1.0.1') throw new Error('Expected pinned @mediapipe/tasks-vision 1.0.1');
const url = 'https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task';
const response = await fetch(url, { redirect: 'error', signal: AbortSignal.timeout(60_000) });
if (!response.ok) throw new Error(`Model download failed: HTTP ${response.status}`);
const chunks = []; let size = 0;
for await (const chunk of response.body) {
  size += chunk.length;
  if (size > 30 * 1024 * 1024) { throw new Error('Model exceeds expected 30 MiB limit'); }
  chunks.push(chunk);
}
const bytes = Buffer.concat(chunks);
const out = path.resolve(values.out), wasmOut = path.join(out, 'wasm');
await mkdir(wasmOut, { recursive: true });
await writeFile(path.join(out, 'face_landmarker.task'), bytes, { flag: 'wx' });
const wasmSource = path.join(path.dirname(packagePath), 'wasm');
for (const entry of await readdir(wasmSource, { withFileTypes: true })) {
  if (entry.isFile() && /\.(wasm|js)$/.test(entry.name)) await copyFile(path.join(wasmSource, entry.name), path.join(wasmOut, entry.name), constants.COPYFILE_EXCL);
}
const licenseUrl = 'https://www.apache.org/licenses/LICENSE-2.0.txt';
const licenseResponse = await fetch(licenseUrl, { redirect: 'error', signal: AbortSignal.timeout(30_000) });
if (!licenseResponse.ok) throw new Error('Apache-2.0 license download failed');
const license = await licenseResponse.text();
if (license.length > 30_000 || !license.includes('Apache License')) throw new Error('Unexpected license response');
await writeFile(path.join(out, 'MEDIAPIPE-LICENSE'), license, { flag: 'wx' });
await writeFile(path.join(out, 'manifest.json'), JSON.stringify({ sdk: installed.version, modelUrl: url, modelSha256: createHash('sha256').update(bytes).digest('hex'), bytes: size, documentation: 'https://developers.google.com/edge/mediapipe/solutions/vision/face_landmarker', cameraFramesUploaded: false }, null, 2) + '\n', { flag: 'wx' });
console.log(JSON.stringify({ out, modelBytes: size, modelSha256: createHash('sha256').update(bytes).digest('hex') }));
