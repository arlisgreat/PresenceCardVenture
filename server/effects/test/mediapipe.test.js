import assert from 'node:assert/strict';
import test from 'node:test';
import { createMediaPipeDetector, createLiveTracker } from '../src/mediapipe.js';
import { installFaceWorker, restrictWorkerFetch } from '../src/face-worker.js';

const assets = { wasmRoot: 'https://presence.test/vendor/mediapipe/wasm/', modelAssetPath: 'https://presence.test/vendor/mediapipe/face_landmarker.task' };
const points = () => Array.from({ length: 478 }, () => ({ x: 0.5, y: 0.4, z: 0.01 }));
const bitmap = () => ({ width: 320, height: 240, closed: 0, close() { this.closed++; } });

function fakeSdk(result = { faceLandmarks: [points()] }) {
  const calls = [];
  const detector = {
    detectForVideo(frame, timestampMs) { calls.push(['detect', frame, timestampMs]); if (result instanceof Error) throw result; return result; },
    close() { calls.push(['close']); },
  };
  const vision = {
    FilesetResolver: { async forVisionTasks(root, useModule) { calls.push(['files', root, useModule]); return { fake: true }; } },
    FaceLandmarker: { async createFromOptions(files, options) { calls.push(['create', files, options]); return detector; } },
  };
  return { vision, calls };
}

class FakeWorker {
  listeners = new Map();
  posted = [];
  terminated = 0;
  autoReady = true;
  throwOnFrame = false;
  addEventListener(type, listener) { if (!this.listeners.has(type)) this.listeners.set(type, new Set()); this.listeners.get(type).add(listener); }
  removeEventListener(type, listener) { this.listeners.get(type)?.delete(listener); }
  postMessage(message, transfer) {
    if (message.type === 'frame' && this.throwOnFrame) throw new Error('untransferable');
    this.posted.push({ message, transfer });
    if (message.type === 'init' && this.autoReady) queueMicrotask(() => this.emit({ type: 'ready' }));
  }
  emit(data) { for (const listener of this.listeners.get('message') ?? []) listener({ data }); }
  event(type) { for (const listener of this.listeners.get(type) ?? []) listener({ preventDefault() {} }); }
  terminate() { this.terminated++; }
  listenerCount() { return [...this.listeners.values()].reduce((sum, set) => sum + set.size, 0); }
}

async function trackerFixture(overrides = {}) {
  const worker = new FakeWorker();
  let factoryArguments;
  const tracker = await createLiveTracker({ ...assets, workerFactory: (...args) => { factoryArguments = args; return worker; }, ...overrides });
  return { worker, tracker, factoryArguments };
}

test('worker-side factory sets explicit CPU/video/single-face thresholds without invented confidence', async () => {
  const { vision, calls } = fakeSdk();
  const detector = await createMediaPipeDetector(vision, assets);
  assert.deepEqual(calls[0], ['files', 'https://presence.test/vendor/mediapipe/wasm', true]);
  assert.deepEqual(calls[1][2], {
    baseOptions: { modelAssetPath: assets.modelAssetPath, delegate: 'CPU' },
    runningMode: 'VIDEO', numFaces: 1,
    minFaceDetectionConfidence: 0.6, minFacePresenceConfidence: 0.6, minTrackingConfidence: 0.6,
    outputFaceBlendshapes: false, outputFacialTransformationMatrixes: false,
  });
  const frame = bitmap();
  const result = detector.detect(frame, 0);
  assert.equal(result.trackingAccepted, true);
  assert.equal(result.landmarks.length, 478);
  assert.equal('confidence' in result, false);
  assert.equal(frame.closed, 0, 'factory borrows frames; worker owns close');
  detector.close();
  detector.close();
  assert.equal(calls.filter(([call]) => call === 'close').length, 1);
  assert.throws(() => detector.detect(frame, 1), /closed/);
});

test('installed MediaPipe resolver selects self-hosted ESM loaders without model or network calls', async () => {
  const { FilesetResolver } = await import('@mediapipe/tasks-vision');
  const files = await FilesetResolver.forVisionTasks('https://presence.test/vendor/mediapipe/wasm', true);
  assert.deepEqual(files, {
    wasmLoaderPath: 'https://presence.test/vendor/mediapipe/wasm/vision_wasm_module_internal.js',
    wasmBinaryPath: 'https://presence.test/vendor/mediapipe/wasm/vision_wasm_module_internal.wasm',
  });
});

test('detector rejects nonmonotonic timestamps and clears absent or malformed faces', async () => {
  for (const faceLandmarks of [[], [points().slice(1)], [[{ x: NaN, y: 0 }]], [points().map((point, i) => i ? point : { x: Infinity, y: 0 })]]) {
    const { vision } = fakeSdk({ faceLandmarks });
    const detector = await createMediaPipeDetector(vision, assets);
    assert.deepEqual(detector.detect(bitmap(), 10), { trackingAccepted: false, landmarks: [] });
    for (const timestamp of [10, 9, -1, NaN, Infinity]) assert.throws(() => detector.detect(bitmap(), timestamp), /timestamps/);
    detector.close();
  }
});

test('assets are explicit, same-origin and resolved before worker initialization', async () => {
  const { worker, tracker, factoryArguments } = await trackerFixture({ modelAssetPath: '/models/face.task' });
  assert.equal(factoryArguments[0].pathname.endsWith('/face-worker.js'), true);
  assert.equal(factoryArguments[1].type, 'module');
  assert.deepEqual(worker.posted[0].message, { type: 'init', wasmRoot: 'https://presence.test/vendor/mediapipe/wasm', modelAssetPath: 'https://presence.test/models/face.task' });
  tracker.close();
  for (const config of [
    {}, { ...assets, modelAssetPath: 'https://external.example/face.task' },
    { ...assets, wasmRoot: 'data:text/plain,no' }, { ...assets, wasmRoot: 'file:///tmp/wasm' },
    { ...assets, wasmRoot: `${assets.wasmRoot}?key=secret` }, { ...assets, modelAssetPath: 'https://user:password@presence.test/face.task' },
  ]) {
    await assert.rejects(createLiveTracker({ ...config, workerFactory: () => new FakeWorker() }));
  }
});

test('UI-thread bridge transfers one bitmap at a time and closes busy/throttled drops', async () => {
  const { worker, tracker } = await trackerFixture({ maxFps: 10 });
  try {
    const first = bitmap();
    const pending = tracker.detect(first, 0);
    const frameMessage = worker.posted[1];
    assert.equal(frameMessage.message.type, 'frame');
    assert.deepEqual(frameMessage.transfer, [first]);
    const busy = bitmap();
    assert.deepEqual(await tracker.detect(busy, 20), { status: 'busy', trackingAccepted: false, landmarks: [] });
    assert.equal(busy.closed, 1);
    assert.equal(worker.posted.length, 2);
    worker.emit({ type: 'result', id: frameMessage.message.id, trackingAccepted: true, landmarks: points() });
    const result = await pending;
    assert.equal(result.status, 'ok');
    assert.equal(result.trackingAccepted, true);
    assert.equal('confidence' in result, false);
    const throttled = bitmap();
    assert.equal((await tracker.detect(throttled, 50)).status, 'throttled');
    assert.equal(throttled.closed, 1);
    const next = tracker.detect(bitmap(), 100);
    worker.emit({ type: 'result', id: worker.posted[2].message.id, trackingAccepted: false, landmarks: [] });
    assert.deepEqual(await next, { status: 'ok', trackingAccepted: false, landmarks: [] });
  } finally { tracker.close(); }
});

test('UI bridge enforces monotonic submitted timestamps and clears malformed worker landmarks', async () => {
  const { worker, tracker } = await trackerFixture();
  try {
    const first = tracker.detect(bitmap(), 100);
    worker.emit({ type: 'result', id: 1, trackingAccepted: true, landmarks: points().slice(1) });
    assert.deepEqual(await first, { status: 'ok', trackingAccepted: false, landmarks: [] });
    for (const timestamp of [100, 99, -1, Infinity, NaN]) {
      const rejected = bitmap();
      await assert.rejects(tracker.detect(rejected, timestamp), /timestamps/);
      assert.equal(rejected.closed, 1);
    }
    const invalid = bitmap();
    invalid.width = 0;
    await assert.rejects(tracker.detect(invalid, 200), /ImageBitmap/);
    assert.equal(invalid.closed, 1);
  } finally { tracker.close(); }
});

test('close terminates, detaches handlers, rejects in-flight work and consumes later frames', async () => {
  const { worker, tracker } = await trackerFixture();
  const pending = tracker.detect(bitmap(), 0);
  const rejected = assert.rejects(pending, { code: 'CLOSED' });
  tracker.close();
  tracker.close();
  await rejected;
  assert.equal(worker.terminated, 1);
  assert.equal(worker.listenerCount(), 0);
  const later = bitmap();
  await assert.rejects(tracker.detect(later, 100), { code: 'CLOSED' });
  assert.equal(later.closed, 1);
});

test('initialization and frame timeouts stop the worker and reject clearly', async () => {
  const starting = new FakeWorker();
  starting.autoReady = false;
  await assert.rejects(createLiveTracker({ ...assets, workerFactory: () => starting, initTimeoutMs: 10 }), { code: 'INIT_TIMEOUT' });
  assert.equal(starting.terminated, 1);
  assert.equal(starting.listenerCount(), 0);
  const { worker, tracker } = await trackerFixture({ frameTimeoutMs: 10 });
  await assert.rejects(tracker.detect(bitmap(), 0), { code: 'FRAME_TIMEOUT' });
  assert.equal(worker.terminated, 1);
  await assert.rejects(tracker.detect(bitmap(), 100), { code: 'CLOSED' });
});

test('worker initialization/inference errors and transfer failures fail closed', async () => {
  const failedInit = new FakeWorker();
  failedInit.autoReady = false;
  const init = createLiveTracker({ ...assets, workerFactory: () => failedInit });
  failedInit.emit({ type: 'error', phase: 'init' });
  await assert.rejects(init, { code: 'INIT_FAILED' });
  assert.equal(failedInit.terminated, 1);

  for (const event of ['inference', 'error', 'messageerror', 'transfer']) {
    const { worker, tracker } = await trackerFixture();
    if (event === 'transfer') worker.throwOnFrame = true;
    const frame = bitmap();
    const pending = tracker.detect(frame, 0);
    const codes = { inference: 'INFERENCE_FAILED', error: 'WORKER_ERROR', messageerror: 'WORKER_MESSAGE_ERROR', transfer: 'FRAME_TRANSFER_FAILED' };
    const rejected = assert.rejects(pending, { code: codes[event] });
    if (event === 'inference') worker.emit({ type: 'error', phase: 'frame', id: 1 });
    else if (event !== 'transfer') worker.event(event);
    await rejected;
    assert.equal(worker.terminated, 1);
    assert.equal(worker.listenerCount(), 0);
    if (event === 'transfer') assert.equal(frame.closed, 1);
  }
});

test('invalid tracker options are rejected before constructing a worker', async () => {
  for (const maxFps of [0, -1, 61, NaN, Infinity]) await assert.rejects(createLiveTracker({ ...assets, maxFps, workerFactory: () => new FakeWorker() }), /maxFps/);
  for (const timeout of [0, -1, 120001, Infinity]) {
    await assert.rejects(createLiveTracker({ ...assets, initTimeoutMs: timeout, workerFactory: () => new FakeWorker() }), /initTimeoutMs/);
    await assert.rejects(createLiveTracker({ ...assets, frameTimeoutMs: timeout, workerFactory: () => new FakeWorker() }), /frameTimeoutMs/);
  }
});

class FakeScope {
  listeners = new Set();
  posted = [];
  closed = 0;
  addEventListener(type, listener) { assert.equal(type, 'message'); this.listeners.add(listener); }
  removeEventListener(type, listener) { this.listeners.delete(listener); }
  postMessage(message) { this.posted.push(message); }
  async receive(message) { await Promise.all([...this.listeners].map((listener) => listener({ data: message }))); }
  close() { this.closed++; }
}

test('worker owns transferred frames and closes them in success/error paths', async () => {
  for (const result of [{ faceLandmarks: [points()] }, new Error('model failure')]) {
    const scope = new FakeScope();
    const { vision, calls } = fakeSdk(result);
    const dispose = installFaceWorker(scope, async () => vision);
    const early = bitmap();
    await scope.receive({ type: 'frame', id: 1, bitmap: early, timestampMs: 0 });
    assert.equal(early.closed, 1);
    assert.equal(scope.posted.at(-1).type, 'error');
    await scope.receive({ type: 'init', ...assets });
    assert.deepEqual(scope.posted.at(-1), { type: 'ready' });
    const frame = bitmap();
    await scope.receive({ type: 'frame', id: 2, bitmap: frame, timestampMs: 1 });
    assert.equal(frame.closed, 1);
    assert.equal(scope.posted.at(-1).type, result instanceof Error ? 'error' : 'result');
    await scope.receive({ type: 'close' });
    assert.equal(scope.closed, 1);
    assert.equal(calls.filter(([name]) => name === 'close').length, 1);
    assert.equal(scope.listeners.size, 0);
    dispose();
  }
});

test('worker init failure reports no raw SDK details and late initialization is disposed', async () => {
  const failedScope = new FakeScope();
  const disposeFailed = installFaceWorker(failedScope, async () => { throw new Error('private URL or SDK detail'); });
  await failedScope.receive({ type: 'init', ...assets });
  assert.deepEqual(failedScope.posted, [{ type: 'error', phase: 'init' }]);
  disposeFailed();

  const scope = new FakeScope();
  const { vision, calls } = fakeSdk();
  let release;
  const loaded = new Promise((resolve) => { release = resolve; });
  const dispose = installFaceWorker(scope, () => loaded);
  const init = scope.receive({ type: 'init', ...assets });
  dispose();
  release(vision);
  await init;
  assert.equal(scope.posted.length, 0);
  assert.equal(calls.filter(([name]) => name === 'close').length, 0, 'closed worker must not create a late detector');
});

test('dedicated worker fetch permits only configured asset reads and blocks SDK telemetry/uploads', async () => {
  const calls = [];
  const scope = { async fetch(input, init) { calls.push({ input, init }); return { ok: true }; } };
  const original = scope.fetch;
  const restore = restrictWorkerFetch(scope, { ...assets, wasmRoot: assets.wasmRoot.replace(/\/$/, '') });
  await scope.fetch(assets.modelAssetPath);
  await scope.fetch(`${assets.wasmRoot}vision_wasm_module_internal.wasm`, { method: 'GET', credentials: 'same-origin' });
  assert.equal(calls.length, 2);
  assert.ok(calls.every((call) => call.init.redirect === 'error'));
  for (const [url, init] of [
    ['https://odml.pa.googleapis.com/v1/log', { method: 'POST', body: 'telemetry' }],
    ['https://presence.test/upload', { method: 'POST', body: 'photo' }],
    [assets.modelAssetPath, { method: 'POST' }],
    [assets.modelAssetPath, { body: 'payload' }],
    [`${assets.wasmRoot}unexpected.js`, {}],
  ]) await assert.rejects(scope.fetch(url, init), /local-asset policy/);
  assert.equal(calls.length, 2, 'blocked requests never call the original network function');
  restore();
  assert.equal(scope.fetch, original);
});
