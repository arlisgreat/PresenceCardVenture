import assert from 'node:assert/strict';
import test from 'node:test';
import { buildFaceOverlays, renderFaceOverlays, createOverlaySmoother, FACE_ANCHORS } from '../src/live.js';

const options = { width: 640, height: 480, style: 'cheek-stars', trackingAccepted: true };

function face() {
  const landmarks = Array.from({ length: 478 }, () => ({ x: 0.5, y: 0.5, z: 0 }));
  landmarks[33] = { x: 0.36, y: 0.43, z: 0 };
  landmarks[263] = { x: 0.64, y: 0.43, z: 0 };
  landmarks[1] = { x: 0.50, y: 0.53, z: -0.02 };
  landmarks[234] = { x: 0.25, y: 0.53, z: 0 };
  landmarks[454] = { x: 0.75, y: 0.53, z: 0 };
  return landmarks;
}

test('tracking acceptance is explicit; no fabricated confidence is required or accepted as a substitute', () => {
  assert.equal(buildFaceOverlays(face(), options).length, 4);
  for (const trackingAccepted of [undefined, false, 1, 'true']) {
    assert.deepEqual(buildFaceOverlays(face(), { ...options, trackingAccepted }), []);
    assert.deepEqual(buildFaceOverlays(face(), { ...options, trackingAccepted, confidence: 1 }), []);
  }
  for (const confidence of [NaN, Infinity, -1, 1.01, 0.64, null, '1']) {
    assert.deepEqual(buildFaceOverlays(face(), { ...options, confidence }), []);
  }
  assert.equal(buildFaceOverlays(face(), { ...options, confidence: 0.65 }).length, 4);
  assert.equal(buildFaceOverlays(face(), { ...options, confidence: 0.6, minConfidence: 0.6 }).length, 4);
  for (const minConfidence of [NaN, -1, 1.1]) assert.deepEqual(buildFaceOverlays(face(), { ...options, minConfidence }), []);
});

test('wrong topology, absent face, invalid anchors or degenerate geometry hide overlays', () => {
  for (const input of [null, [], face().slice(0, 468), [...face(), {}]]) assert.deepEqual(buildFaceOverlays(input, options), []);
  for (const index of Object.values(FACE_ANCHORS)) {
    for (const invalid of [undefined, { x: NaN, y: 0.5 }, { x: 0.5, y: Infinity }, { x: 0.5, y: 0.5, z: NaN }]) {
      const landmarks = face();
      landmarks[index] = invalid;
      assert.deepEqual(buildFaceOverlays(landmarks, options), []);
    }
  }
  const collapsed = Array.from({ length: 478 }, () => ({ x: 0.5, y: 0.5 }));
  assert.deepEqual(buildFaceOverlays(collapsed, options), []);
});

test('styles are deterministic, original vector primitives and leave landmarks untouched', () => {
  const landmarks = face();
  const before = structuredClone(landmarks);
  for (const style of ['cheek-stars', 'orbit']) {
    const commands = buildFaceOverlays(landmarks, { ...options, style });
    assert.deepEqual(commands, buildFaceOverlays(landmarks, { ...options, style }));
    assert.equal(commands.length, style === 'cheek-stars' ? 4 : 6);
    assert.equal(new Set(commands.map((command) => command.id)).size, commands.length);
    assert.ok(commands.every((command) => ['star', 'ellipse'].includes(command.type)));
    assert.ok(commands.every((command) => command.opacity > 0 && command.opacity <= 1));
  }
  assert.deepEqual(landmarks, before);
});

test('mirroring changes horizontal position and rotation exactly once', () => {
  for (const style of ['cheek-stars', 'orbit']) {
    const normal = buildFaceOverlays(face(), { ...options, style });
    const mirrored = buildFaceOverlays(face(), { ...options, style, mirrored: true });
    normal.forEach((command, i) => {
      assert.equal(mirrored[i].x, options.width - command.x);
      assert.equal(mirrored[i].y, command.y);
      assert.equal(mirrored[i].rotation, -command.rotation);
      assert.equal(mirrored[i].id, command.id);
    });
  }
});

test('clipped anchors produce finite commands contained in the frame', () => {
  for (const [width, height] of [[320, 240], [24, 64], [64, 24]]) {
    for (const style of ['cheek-stars', 'orbit']) {
      const landmarks = face();
      landmarks[33] = { x: -0.3, y: 0.2 };
      landmarks[263] = { x: 1.2, y: 0.3 };
      landmarks[234] = { x: -0.4, y: 1.2 };
      landmarks[454] = { x: 1.3, y: 1.4 };
      const commands = buildFaceOverlays(landmarks, { ...options, width, height, style });
      assert.ok(commands.length > 0);
      for (const command of commands) {
        assert.ok(Object.values(command).filter((value) => typeof value === 'number').every(Number.isFinite));
        if (command.type === 'star') {
          assert.ok(command.x - command.radius >= -1e-9 && command.x + command.radius <= width + 1e-9);
          assert.ok(command.y - command.radius >= -1e-9 && command.y + command.radius <= height + 1e-9);
        } else {
          const xExtent = Math.hypot(command.radiusX * Math.cos(command.rotation), command.radiusY * Math.sin(command.rotation)) + command.lineWidth / 2;
          const yExtent = Math.hypot(command.radiusX * Math.sin(command.rotation), command.radiusY * Math.cos(command.rotation)) + command.lineWidth / 2;
          assert.ok(command.x - xExtent >= -1e-9 && command.x + xExtent <= width + 1e-9);
          assert.ok(command.y - yExtent >= -1e-9 && command.y + yExtent <= height + 1e-9);
        }
      }
    }
  }
});

test('invalid viewport/style/mirror options hide overlays without NaN commands', () => {
  for (const dimension of [0, -1, NaN, Infinity, 32769, 0.5]) {
    assert.deepEqual(buildFaceOverlays(face(), { ...options, width: dimension }), []);
    assert.deepEqual(buildFaceOverlays(face(), { ...options, height: dimension }), []);
  }
  assert.deepEqual(buildFaceOverlays(face(), { ...options, style: 'face-warp' }), []);
  assert.deepEqual(buildFaceOverlays(face(), { ...options, mirrored: 1 }), []);
  assert.deepEqual(buildFaceOverlays(face()), []);
});

class FakeCanvas {
  globalAlpha = 0.6;
  fillStyle = '#000000';
  strokeStyle = '#ffffff';
  lineWidth = 2;
  stack = [];
  events = [];
  save() { this.stack.push([this.globalAlpha, this.fillStyle, this.strokeStyle, this.lineWidth]); }
  restore() { [this.globalAlpha, this.fillStyle, this.strokeStyle, this.lineWidth] = this.stack.pop(); }
  beginPath() { this.events.push(['begin']); }
  moveTo(...args) { this.events.push(['move', ...args]); }
  lineTo(...args) { this.events.push(['line', ...args]); }
  closePath() { this.events.push(['close']); }
  fill() { this.events.push(['fill', this.globalAlpha, this.fillStyle]); }
  ellipse(...args) { this.events.push(['ellipse', ...args]); }
  stroke() { this.events.push(['stroke', this.globalAlpha, this.strokeStyle]); }
}

test('renderer draws without DOM globals and preserves Canvas2D state', () => {
  const ctx = new FakeCanvas();
  const commands = buildFaceOverlays(face(), { ...options, style: 'orbit' });
  const before = structuredClone(commands);
  assert.equal(renderFaceOverlays(ctx, commands), 6);
  assert.equal(ctx.events.filter((event) => event[0] === 'fill').length, 5);
  assert.equal(ctx.events.filter((event) => event[0] === 'stroke').length, 1);
  assert.equal(ctx.events.find((event) => event[0] === 'stroke')[1], 0.6 * commands[0].opacity);
  assert.deepEqual([ctx.globalAlpha, ctx.fillStyle, ctx.strokeStyle, ctx.lineWidth], [0.6, '#000000', '#ffffff', 2]);
  assert.equal(ctx.stack.length, 0);
  assert.deepEqual(commands, before);
  assert.equal(renderFaceOverlays(null, commands), 0);
  assert.equal(renderFaceOverlays({}, commands), 0);
});

test('renderer skips invalid commands and restores context when drawing fails', () => {
  const ctx = new FakeCanvas();
  const [valid] = buildFaceOverlays(face(), options);
  const invalid = [null, { ...valid, x: NaN }, { ...valid, radius: Infinity }, { ...valid, radius: 1e300 }, { ...valid, fill: 'not-a-color' }, { ...valid, opacity: -1 }, { ...valid, points: 999 }];
  assert.equal(renderFaceOverlays(ctx, [...invalid, valid]), 1);
  ctx.fill = () => { throw new Error('canvas lost'); };
  assert.throws(() => renderFaceOverlays(ctx, [valid]), /canvas lost/);
  assert.equal(ctx.globalAlpha, 0.6);
  assert.equal(ctx.stack.length, 0);
});

test('temporal controller smooths without mutating commands or exposing internal state', () => {
  const controller = createOverlaySmoother({ alpha: 0.5 });
  const original = buildFaceOverlays(face(), options);
  const moved = original.map((command) => ({ ...command, x: command.x + 20, y: command.y + 10 }));
  const before = structuredClone(original);
  assert.deepEqual(controller.update(original, 0), original);
  const second = controller.update(moved, 16);
  second.forEach((command, i) => {
    assert.equal(command.x, original[i].x + 10);
    assert.equal(command.y, original[i].y + 5);
  });
  second[0].x = 65535;
  const third = controller.update(moved, 32);
  assert.equal(third[0].x, original[0].x + 15);
  assert.deepEqual(original, before);
});

test('temporal controller hides/resets on absence, stale timestamps, changed styles and explicit reset', () => {
  const original = buildFaceOverlays(face(), options);
  const moved = original.map((command) => ({ ...command, x: command.x + 20 }));
  const controller = createOverlaySmoother({ resetAfterMs: 100 });
  controller.update(original, 0);
  assert.deepEqual(controller.update([], 16), []);
  assert.deepEqual(controller.update(moved, 32), moved);
  assert.deepEqual(controller.update(original, 300), original);
  assert.deepEqual(controller.update(moved, 200), moved);
  const orbit = buildFaceOverlays(face(), { ...options, style: 'orbit' });
  assert.deepEqual(controller.update(orbit, 210), orbit);
  controller.reset();
  assert.deepEqual(controller.update(original, 220), original);
  assert.deepEqual(controller.update([{ ...original[0], x: NaN }], 230), []);
});

test('rotation smoothing takes the short arc and rejects invalid controller parameters', () => {
  const [star] = buildFaceOverlays(face(), options);
  const controller = createOverlaySmoother({ alpha: 0.5 });
  controller.update([{ ...star, rotation: Math.PI - 0.01 }], 0);
  const [next] = controller.update([{ ...star, rotation: -Math.PI + 0.01 }], 16);
  assert.ok(Math.abs(next.rotation - Math.PI) < 1e-12);
  for (const alpha of [0, -1, 1.1, NaN]) assert.throws(() => createOverlaySmoother({ alpha }), /alpha/);
  for (const resetAfterMs of [0, -1, NaN]) assert.throws(() => createOverlaySmoother({ resetAfterMs }), /resetAfterMs/);
  for (const timestamp of [-1, NaN, Infinity]) assert.throws(() => controller.update([star], timestamp), /timestampMs/);
});
