import assert from 'node:assert/strict';
import test from 'node:test';
import { applyLookToRgba, transformRgb, PRESET_VERSION } from '../src/pixels.js';
import { PRESETS, getPreset } from '../src/presets.js';

function fixture(width = 16, height = 12) {
  const data = new Uint8Array(width * height * 4);
  for (let i = 0; i < data.length; i += 4) {
    data[i] = (i * 37 + 83) % 256;
    data[i + 1] = (i * 11 + 145) % 256;
    data[i + 2] = (i * 23 + 203) % 256;
    data[i + 3] = (i / 4) % 256;
  }
  return { data, width, height };
}

test('presets have stable legacy IDs and are deeply frozen', () => {
  assert.deepEqual(PRESETS.map((preset) => preset.id), ['none', 'warm', 'bw', 'film', 'vivid']);
  assert.ok(Object.isFrozen(PRESETS));
  for (const preset of PRESETS) {
    assert.equal(preset.version, PRESET_VERSION);
    assert.equal(getPreset(preset.id), preset);
    assert.ok(Object.isFrozen(preset));
    assert.ok(Object.isFrozen(preset.toneCurve));
    assert.ok(Object.isFrozen(preset.toneCurve[0]));
    assert.ok(Object.isFrozen(preset.glow));
    assert.ok(Object.isFrozen(preset.glow.tint));
  }
  assert.throws(() => getPreset('ccd'), /Unknown Presence preset/);
});

test('none and zero intensity preserve every byte in a new buffer', () => {
  const image = fixture();
  for (const options of [{}, ...PRESETS.map(({ id }) => ({ presetId: id, intensity: 0 }))]) {
    const result = applyLookToRgba(image, options);
    assert.ok(result instanceof Uint8ClampedArray);
    assert.notEqual(result.buffer, image.data.buffer);
    assert.deepEqual(Array.from(result), Array.from(image.data));
  }
});

test('all looks are deterministic, do not mutate source, and preserve alpha/transparent RGB', () => {
  const image = fixture();
  const before = image.data.slice();
  for (const { id } of PRESETS) {
    const options = { presetId: id, seed: 427 };
    const first = applyLookToRgba(image, options);
    assert.deepEqual(first, applyLookToRgba(image, options));
    assert.deepEqual(image.data, before);
    for (let i = 0; i < first.length; i += 4) {
      assert.equal(first[i + 3], image.data[i + 3]);
      if (image.data[i + 3] === 0) assert.deepEqual(first.slice(i, i + 4), new Uint8ClampedArray(image.data.slice(i, i + 4)));
    }
  }
});

test('grain uses the provided seed and works with Uint8ClampedArray input', () => {
  const image = fixture(32, 24);
  image.data = new Uint8ClampedArray(image.data);
  assert.notDeepEqual(applyLookToRgba(image, { presetId: 'film', seed: 1 }), applyLookToRgba(image, { presetId: 'film', seed: 2 }));
  assert.deepEqual(applyLookToRgba(image, { presetId: 'film', seed: 1 + 2 ** 32 }), applyLookToRgba(image, { presetId: 'film', seed: 1 }));
});

test('full black-and-white preserves monochrome equality including all spatial effects', () => {
  const result = applyLookToRgba(fixture(), { presetId: 'bw' });
  for (let i = 0; i < result.length; i += 4) {
    if (result[i + 3] === 0) continue;
    assert.equal(result[i], result[i + 1]);
    assert.equal(result[i + 1], result[i + 2]);
  }
});

test('intensity blends the complete look, including spatial effects, once', () => {
  const image = fixture();
  for (const { id } of PRESETS) {
    const full = applyLookToRgba(image, { presetId: id, seed: 9 });
    const half = applyLookToRgba(image, { presetId: id, intensity: 0.5, seed: 9 });
    for (let i = 0; i < half.length; i++) {
      assert.ok(Math.abs(half[i] - (image.data[i] + full[i]) / 2) <= 0.75);
    }
  }
});

test('spatial code handles 1×1, single-row and single-column images', () => {
  for (const [width, height] of [[1, 1], [1, 17], [19, 1]]) {
    const image = fixture(width, height);
    for (const { id } of PRESETS) {
      const output = applyLookToRgba(image, { presetId: id });
      assert.equal(output.length, image.data.length);
      assert.ok(output.every((value) => Number.isFinite(value)));
    }
  }
});

test('rejects invalid pixel buffers, dimensions, intensities and seeds even for none', () => {
  const image = fixture();
  for (const data of [[], new Uint16Array(image.data.length), image.data.subarray(4), null]) {
    assert.throws(() => applyLookToRgba({ ...image, data }), /RGBA byte array/);
  }
  for (const dimension of [0, -1, 1.1, Infinity, NaN, Number.MAX_SAFE_INTEGER]) {
    assert.throws(() => applyLookToRgba({ ...image, width: dimension }), /integers|RGBA byte array/);
    assert.throws(() => applyLookToRgba({ ...image, height: dimension }), /integers|RGBA byte array/);
  }
  for (const intensity of [-1, 1.01, NaN, Infinity, '0.5', null]) {
    assert.throws(() => applyLookToRgba(image, { intensity }), /intensity/);
    assert.throws(() => transformRgb([10, 20, 30], 'none', intensity), /intensity/);
  }
  for (const seed of [NaN, Infinity, -Infinity, 1.9, Number.MAX_SAFE_INTEGER + 1, '1', null]) assert.throws(() => applyLookToRgba(image, { seed }), /seed/);
  assert.throws(() => applyLookToRgba(image, { presetId: 'missing' }), /Unknown Presence preset/);
});

test('RGB transform is continuous-valued, non-mutating, bounded and linearly blendable', () => {
  const rgb = [92.5, 115.75, 162.25];
  assert.deepEqual(transformRgb(rgb), rgb);
  assert.notEqual(transformRgb(rgb), rgb);
  for (const { id } of PRESETS) {
    const full = transformRgb(rgb, id);
    const half = transformRgb(rgb, id, 0.5);
    assert.deepEqual(transformRgb(rgb, id, 0), rgb);
    for (let channel = 0; channel < 3; channel++) {
      assert.ok(full[channel] >= 0 && full[channel] <= 255);
      assert.equal(half[channel], rgb[channel] + (full[channel] - rgb[channel]) * 0.5);
    }
    for (const sample of [[0, 0, 0], [255, 255, 255], [255, 0, 255]]) {
      assert.ok(transformRgb(sample, id).every((value) => Number.isFinite(value) && value >= 0 && value <= 255));
    }
  }
  assert.deepEqual(rgb, [92.5, 115.75, 162.25]);
  for (const invalid of [[0, 1], [0, 1, 2, 3], [0, 0, 256], [-1, 0, 0], [NaN, 1, 2], ['1', 2, 3], null]) {
    assert.throws(() => transformRgb(invalid, 'warm'), /rgb/);
  }
});
