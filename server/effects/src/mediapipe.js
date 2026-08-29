import { resolveTrackingAssets, cleanFaceLandmarks, checkFrameTimestamp } from './face-worker.js';
export { createMediaPipeDetector } from './face-worker.js';

function trackerError(code, message) {
  return Object.assign(new Error(message), { name: 'LiveTrackerError', code });
}

function closeBitmap(bitmap) {
  try { bitmap?.close?.(); } catch { /* Detached frames may already be closed. */ }
}

/**
 * Browser UI-thread bridge. Only the module Worker imports/runs MediaPipe.
 * detect() consumes every bitmap, including dropped or rejected frames.
 * busy/throttled means no inference occurred; update overlays only on status=ok.
 */
export async function createLiveTracker({
  wasmRoot, modelAssetPath, maxFps = 15, workerFactory,
  initTimeoutMs = 30000, frameTimeoutMs = 5000,
} = {}) {
  const assets = resolveTrackingAssets(wasmRoot, modelAssetPath);
  if (!Number.isFinite(maxFps) || maxFps <= 0 || maxFps > 60) throw new RangeError('maxFps must be in (0, 60]');
  for (const [name, value] of Object.entries({ initTimeoutMs, frameTimeoutMs })) {
    if (!Number.isFinite(value) || value <= 0 || value > 120000) throw new RangeError(`${name} must be in (0, 120000]`);
  }
  if (workerFactory !== undefined && typeof workerFactory !== 'function') throw new TypeError('workerFactory must be a function');
  if (!workerFactory && typeof Worker !== 'function') throw new Error('Module Workers are required for Live Effects');
  const worker = workerFactory
    ? workerFactory(new URL('./face-worker.js', import.meta.url), { type: 'module', name: 'presence-face-tracker' })
    : new Worker(new URL('./face-worker.js', import.meta.url), { type: 'module', name: 'presence-face-tracker' });
  if (!worker || !['postMessage', 'terminate', 'addEventListener', 'removeEventListener'].every((key) => typeof worker[key] === 'function')) {
    worker?.terminate?.();
    throw new TypeError('workerFactory must return a Worker-compatible object');
  }

  let phase = 'starting';
  let pending = null;
  let lastSubmittedTimestamp = -Infinity;
  let nextId = 1;
  let initTimer;
  let frameTimer;
  let resolveReady;
  let rejectReady;
  const ready = new Promise((resolve, reject) => { resolveReady = resolve; rejectReady = reject; });

  function fail(error) {
    if (phase === 'closed') return;
    const wasStarting = phase === 'starting';
    phase = 'closed';
    clearTimeout(initTimer);
    clearTimeout(frameTimer);
    worker.removeEventListener('message', onMessage);
    worker.removeEventListener('error', onError);
    worker.removeEventListener('messageerror', onMessageError);
    try { worker.terminate(); } catch { /* Cleanup remains idempotent. */ }
    if (wasStarting) rejectReady(error);
    if (pending) {
      const frame = pending;
      pending = null;
      frame.reject(error);
    }
  }

  const tracker = {
    detect(bitmap, timestampMs) {
      if (phase !== 'ready') {
        closeBitmap(bitmap);
        return Promise.reject(trackerError('CLOSED', 'Live tracker is closed'));
      }
      if (!bitmap || typeof bitmap.close !== 'function' || !Number.isFinite(bitmap.width) || bitmap.width <= 0 || !Number.isFinite(bitmap.height) || bitmap.height <= 0) {
        closeBitmap(bitmap);
        return Promise.reject(new TypeError('detect requires an open ImageBitmap'));
      }
      try { checkFrameTimestamp(timestampMs, lastSubmittedTimestamp); } catch (error) {
        closeBitmap(bitmap);
        return Promise.reject(error);
      }
      if (pending) {
        closeBitmap(bitmap);
        return Promise.resolve({ status: 'busy', trackingAccepted: false, landmarks: [] });
      }
      if (timestampMs - lastSubmittedTimestamp < 1000 / maxFps) {
        closeBitmap(bitmap);
        return Promise.resolve({ status: 'throttled', trackingAccepted: false, landmarks: [] });
      }
      return new Promise((resolve, reject) => {
        const id = nextId++;
        pending = { id, resolve, reject };
        lastSubmittedTimestamp = timestampMs;
        frameTimer = setTimeout(() => fail(trackerError('FRAME_TIMEOUT', 'Live tracking timed out; worker stopped')), frameTimeoutMs);
        try {
          worker.postMessage({ type: 'frame', id, bitmap, timestampMs }, [bitmap]);
        } catch {
          closeBitmap(bitmap);
          fail(trackerError('FRAME_TRANSFER_FAILED', 'Could not transfer the frame to the tracking worker'));
        }
      });
    },
    close() { fail(trackerError('CLOSED', 'Live tracker was closed')); },
  };

  function onMessage(event) {
    const data = event.data;
    if (!data || typeof data !== 'object' || phase === 'closed') return;
    if (data.type === 'ready' && phase === 'starting') {
      clearTimeout(initTimer);
      phase = 'ready';
      resolveReady(tracker);
    } else if (data.type === 'result' && phase === 'ready' && pending?.id === data.id) {
      clearTimeout(frameTimer);
      const frame = pending;
      pending = null;
      const landmarks = data.trackingAccepted === true ? cleanFaceLandmarks(data.landmarks) : [];
      frame.resolve({ status: 'ok', trackingAccepted: landmarks.length === 478, landmarks });
    } else if (data.type === 'error' && ((phase === 'starting' && data.phase === 'init') || (phase === 'ready' && data.phase === 'frame' && pending?.id === data.id))) {
      fail(trackerError(phase === 'starting' ? 'INIT_FAILED' : 'INFERENCE_FAILED', 'Face tracking failed; check self-hosted assets and browser compatibility'));
    }
  }
  function onError(event) {
    event.preventDefault?.();
    fail(trackerError('WORKER_ERROR', 'Face tracking worker failed'));
  }
  function onMessageError() { fail(trackerError('WORKER_MESSAGE_ERROR', 'Could not read the tracking worker response')); }

  worker.addEventListener('message', onMessage);
  worker.addEventListener('error', onError);
  worker.addEventListener('messageerror', onMessageError);
  initTimer = setTimeout(() => fail(trackerError('INIT_TIMEOUT', 'Face tracking initialization timed out; worker stopped')), initTimeoutMs);
  try { worker.postMessage({ type: 'init', ...assets }); } catch {
    fail(trackerError('INIT_FAILED', 'Could not initialize the tracking worker'));
  }
  return ready;
}
