import { createServer } from 'node:http';
import { build } from 'esbuild';
import { mkdir, readFile, access } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

// Local integration harness only: no camera, upload, business UI or deployment.
const effectsRoot = fileURLToPath(new URL('../', import.meta.url));
const output = path.join(effectsRoot, 'output/live-smoke');
const port = 5175;
await mkdir(output, { recursive: true });

const client = `
import { createLiveTracker } from '@pvc/effects/mediapipe';
import { buildFaceOverlays, renderFaceOverlays } from '@pvc/effects/live';
const source = document.getElementById('source');
const canvas = document.getElementById('result');
const status = document.getElementById('status');
const run = document.getElementById('run');
const ctx = canvas.getContext('2d');
await source.decode();
canvas.width = source.naturalWidth;
canvas.height = source.naturalHeight;
ctx.drawImage(source, 0, 0);
run.disabled = false;
run.addEventListener('click', async () => {
  run.disabled = true;
  let tracker;
  try {
    status.textContent = '正在加载本机模型与 WASM…';
    const started = performance.now();
    tracker = await createLiveTracker({
      wasmRoot: '/assets/live/wasm', modelAssetPath: '/assets/live/face_landmarker.task',
      initTimeoutMs: 30000, frameTimeoutMs: 10000,
      workerFactory: () => new Worker('/face-worker.js', { type: 'module', name: 'presence-live-smoke' }),
    });
    const initialized = performance.now();
    const frame = await createImageBitmap(source);
    const frameStarted = performance.now();
    const result = await tracker.detect(frame, 1);
    const detected = performance.now();
    const commands = buildFaceOverlays(result.landmarks, {
      width: canvas.width, height: canvas.height, style: 'cheek-stars',
      trackingAccepted: result.trackingAccepted,
    });
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.drawImage(source, 0, 0);
    const drawn = renderFaceOverlays(ctx, commands);
    status.textContent = JSON.stringify({
      status: result.status, trackingAccepted: result.trackingAccepted,
      landmarkCount: result.landmarks.length, overlayCount: drawn,
      initializationMs: Math.round(initialized - started),
      singleFrameInferenceMs: Math.round(detected - frameStarted),
      note: '合成人物图片的本机单帧测试；不是移动端或实时帧率测试。',
      cameraUsed: false, imageUploaded: false,
    }, null, 2);
  } catch (error) {
    status.textContent = JSON.stringify({ status: 'error', code: error.code || error.name, message: error.message }, null, 2);
  } finally {
    tracker?.close();
    run.disabled = false;
  }
});
`;

await Promise.all([
  build({
    stdin: { contents: client, sourcefile: 'live-smoke-client.js', resolveDir: effectsRoot, loader: 'js' },
    outfile: path.join(output, 'main.js'), bundle: true, format: 'esm', platform: 'browser', target: 'es2022', logLevel: 'silent',
  }),
  build({
    entryPoints: [path.join(effectsRoot, 'src/face-worker.js')], outfile: path.join(output, 'face-worker.js'),
    bundle: true, format: 'esm', platform: 'browser', target: 'es2022', logLevel: 'silent',
  }),
]);

const html = `<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Presence Live 模型集成测试</title>
<style>body{max-width:1100px;margin:32px auto;padding:0 20px;font:16px/1.6 system-ui;background:#fbf9f7;color:#3a3f45}button{padding:10px 18px;cursor:pointer}pre{white-space:pre-wrap;background:#fff;padding:16px;border:1px solid #e9e2dd}section{display:flex;gap:16px;flex-wrap:wrap}figure{margin:0;flex:1;min-width:280px}img,canvas{display:block;width:100%;height:auto}figcaption{margin-bottom:8px}</style>
<h1>Live 模型集成测试</h1>
<p>合成人物图片 · 本地单帧 · 不调用摄像头、不上传图片；不代表移动端帧率。</p>
<button id="run" disabled>运行 Live 模型测试</button>
<pre id="status" role="status">等待运行</pre>
<section><figure><figcaption>合成源图</figcaption><img id="source" src="/source.png" alt="用于本地集成测试的合成人物图片"></figure><figure><figcaption>实际模型定位 + 原创星星贴纸</figcaption><canvas id="result"></canvas></figure></section>
<script type="module" src="/main.js"></script></html>`;

// Exact allowlist: no arbitrary path-to-filesystem mapping.
const routes = new Map([
  ['/main.js', [path.join(output, 'main.js'), 'application/javascript']],
  ['/face-worker.js', [path.join(output, 'face-worker.js'), 'application/javascript']],
  ['/source.png', [path.join(effectsRoot, 'examples/source.png'), 'image/png']],
  ['/assets/live/face_landmarker.task', [path.join(effectsRoot, 'assets/live/face_landmarker.task'), 'application/octet-stream']],
  ['/assets/live/MEDIAPIPE-LICENSE', [path.join(effectsRoot, 'assets/live/MEDIAPIPE-LICENSE'), 'text/plain; charset=utf-8']],
  ['/assets/live/manifest.json', [path.join(effectsRoot, 'assets/live/manifest.json'), 'application/json']],
  ['/assets/live/wasm/vision_wasm_module_internal.js', [path.join(effectsRoot, 'assets/live/wasm/vision_wasm_module_internal.js'), 'application/javascript']],
  ['/assets/live/wasm/vision_wasm_module_internal.wasm', [path.join(effectsRoot, 'assets/live/wasm/vision_wasm_module_internal.wasm'), 'application/wasm']],
]);
for (const [file] of routes.values()) {
  try { await access(file); } catch { throw new Error(`Missing local smoke asset: ${path.relative(effectsRoot, file)}; run npm run live-assets first`); }
}

const csp = "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; connect-src 'self'; worker-src 'self'; img-src 'self' blob:; style-src 'self' 'unsafe-inline'; object-src 'none'; base-uri 'none'";
const server = createServer(async (request, response) => {
  response.setHeader('Content-Security-Policy', csp);
  response.setHeader('Permissions-Policy', 'camera=(), microphone=()');
  response.setHeader('X-Content-Type-Options', 'nosniff');
  response.setHeader('Cache-Control', 'no-store');
  if (!['GET', 'HEAD'].includes(request.method) || ![`127.0.0.1:${port}`, `localhost:${port}`].includes(request.headers.host)) {
    response.writeHead(403).end('Forbidden');
    return;
  }
  let pathname;
  try { pathname = new URL(request.url, `http://127.0.0.1:${port}`).pathname; } catch { response.writeHead(400).end('Bad request'); return; }
  if (pathname === '/') {
    response.setHeader('Content-Type', 'text/html; charset=utf-8');
    response.end(request.method === 'HEAD' ? undefined : html);
    return;
  }
  const route = routes.get(pathname);
  if (!route) { response.writeHead(404).end('Not found'); return; }
  try {
    const bytes = await readFile(route[0]);
    response.setHeader('Content-Type', route[1]);
    response.setHeader('Content-Length', bytes.length);
    response.end(request.method === 'HEAD' ? undefined : bytes);
  } catch { response.writeHead(500).end('Local asset unavailable'); }
});

server.on('error', (error) => { console.error(`Live smoke server: ${error.code || error.message}`); process.exitCode = 1; });
server.listen(port, '127.0.0.1', () => console.log(`Presence Live smoke: http://127.0.0.1:${port}/`));
for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, () => server.close());
