/** Shared validation only; this module never imports the UI-thread Worker bridge. */
export function resolveTrackingAssets(wasmRoot, modelAssetPath) {
  if (typeof wasmRoot !== 'string' || !wasmRoot.trim() || typeof modelAssetPath !== 'string' || !modelAssetPath.trim()) throw new TypeError('Explicit wasmRoot and modelAssetPath are required');
  let base;
  try { base = new URL(globalThis.location?.href ?? wasmRoot); } catch { throw new TypeError('Relative asset paths require a browser origin'); }
  if (!['http:', 'https:'].includes(base.protocol)) throw new TypeError('Assets require an HTTP(S) origin');
  const urls = [new URL(wasmRoot, base), new URL(modelAssetPath, base)];
  for (const url of urls) {
    if (url.origin !== base.origin || !['http:', 'https:'].includes(url.protocol) || url.username || url.password || url.hash) throw new TypeError('Model and WASM assets must be same-origin HTTP(S) URLs without credentials or fragments');
  }
  if (urls[0].search) throw new TypeError('wasmRoot must be a directory URL without a query');
  return { wasmRoot: urls[0].href.replace(/\/+$/, ''), modelAssetPath: urls[1].href };
}

export function cleanFaceLandmarks(points) {
  if (!Array.isArray(points) || points.length !== 478) return [];
  if (!points.every((point) => point && Number.isFinite(point.x) && Number.isFinite(point.y) && (point.z === undefined || Number.isFinite(point.z)))) return [];
  return points.map((point) => point.z === undefined ? { x: point.x, y: point.y } : { x: point.x, y: point.y, z: point.z });
}

export function checkFrameTimestamp(timestampMs, previous = -Infinity) {
  if (!Number.isFinite(timestampMs) || timestampMs < 0 || timestampMs <= previous) throw new RangeError('Frame timestamps must be finite, nonnegative and strictly increasing');
}

/** Worker-side only. SDK is injected; frames are borrowed and the caller closes them. */
export async function createMediaPipeDetector(vision, { wasmRoot, modelAssetPath } = {}) {
  const assets = resolveTrackingAssets(wasmRoot, modelAssetPath);
  if (typeof vision?.FilesetResolver?.forVisionTasks !== 'function' || typeof vision?.FaceLandmarker?.createFromOptions !== 'function') throw new TypeError('A compatible MediaPipe Tasks Vision SDK is required');
  // tasks-vision 1.0.1: useModule=true selects the ESM loader for a module Worker.
  const files = await vision.FilesetResolver.forVisionTasks(assets.wasmRoot, true);
  const detector = await vision.FaceLandmarker.createFromOptions(files, {
    baseOptions: { modelAssetPath: assets.modelAssetPath, delegate: 'CPU' },
    runningMode: 'VIDEO', numFaces: 1,
    minFaceDetectionConfidence: 0.6, minFacePresenceConfidence: 0.6, minTrackingConfidence: 0.6,
    outputFaceBlendshapes: false, outputFacialTransformationMatrixes: false,
  });
  let closed = false;
  let previousTimestamp = -Infinity;
  return {
    detect(frame, timestampMs) {
      if (closed) throw new Error('Face detector is closed');
      checkFrameTimestamp(timestampMs, previousTimestamp);
      previousTimestamp = timestampMs;
      const landmarks = cleanFaceLandmarks(detector.detectForVideo(frame, timestampMs)?.faceLandmarks?.[0]);
      return { trackingAccepted: landmarks.length === 478, landmarks };
    },
    close() { if (!closed) { closed = true; detector.close(); } },
  };
}

/**
 * Constrain only this dedicated Worker's fetch. MediaPipe 1.0.1 includes telemetry;
 * allowlist local model/WASM GETs and block telemetry, uploads and redirects.
 * The surrounding page's fetch, credentials and network configuration are untouched.
 */
export function restrictWorkerFetch(scope, assets) {
  if (typeof scope.fetch !== 'function') return () => {};
  const original = scope.fetch;
  const allowed = new Set([
    assets.modelAssetPath,
    `${assets.wasmRoot}/vision_wasm_module_internal.js`,
    `${assets.wasmRoot}/vision_wasm_module_internal.wasm`,
  ]);
  const guarded = (input, init = {}) => {
    let url;
    const method = String(init.method ?? input?.method ?? 'GET').toUpperCase();
    try { url = new URL(input?.url ?? input, assets.modelAssetPath); } catch { return Promise.reject(new Error('Worker request is not an allowed local asset')); }
    if (!allowed.has(url.href) || !['GET', 'HEAD'].includes(method) || init.body != null || input?.body != null) {
      return Promise.reject(new Error('Worker network request blocked by the local-asset policy'));
    }
    return original.call(scope, input, { ...init, method, redirect: 'error' });
  };
  scope.fetch = guarded;
  return () => { if (scope.fetch === guarded) scope.fetch = original; };
}

/** Injectable worker host for camera-free tests. The default SDK is bundled locally. */
export function installFaceWorker(scope, loadVision = () => import('@mediapipe/tasks-vision')) {
  let detector = null;
  let initializing = false;
  let closed = false;
  let busy = false;
  let restoreFetch = () => {};

  function dispose() {
    if (closed) return;
    closed = true;
    scope.removeEventListener('message', onMessage);
    try { detector?.close(); } catch { /* Still release the Worker/network guard. */ }
    detector = null;
    if (!initializing) restoreFetch();
  }

  async function onMessage(event) {
    const message = event.data;
    if (!message || typeof message !== 'object') return;
    if (message.type === 'frame') {
      const bitmap = message.bitmap;
      try {
        if (closed || !detector || busy || !Number.isSafeInteger(message.id) || message.id < 1) throw new Error('Worker not ready');
        busy = true;
        const result = detector.detect(bitmap, message.timestampMs);
        scope.postMessage({ type: 'result', id: message.id, ...result });
      } catch {
        if (!closed) scope.postMessage({ type: 'error', phase: 'frame', id: message.id });
      } finally {
        busy = false;
        try { bitmap?.close?.(); } catch { /* A detached bitmap may already be closed. */ }
      }
    } else if (message.type === 'init' && !closed && !initializing && !detector) {
      initializing = true;
      try {
        const assets = resolveTrackingAssets(message.wasmRoot, message.modelAssetPath);
        restoreFetch = restrictWorkerFetch(scope, assets);
        const vision = await loadVision();
        if (closed) return;
        const created = await createMediaPipeDetector(vision, assets);
        if (closed) { created.close(); return; }
        detector = created;
        scope.postMessage({ type: 'ready' });
      } catch {
        if (!closed) scope.postMessage({ type: 'error', phase: 'init' });
      } finally {
        initializing = false;
        if (closed) restoreFetch();
      }
    } else if (message.type === 'close') {
      dispose();
      scope.close?.();
    }
  }

  scope.addEventListener('message', onMessage);
  return dispose;
}

if (typeof WorkerGlobalScope !== 'undefined' && globalThis instanceof WorkerGlobalScope) {
  installFaceWorker(globalThis);
}
