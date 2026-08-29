import test from 'node:test';
import assert from 'node:assert/strict';
import sharp from 'sharp';
import { renderPhoto, prepareReference, createPhotoPair, rgb565ToRgba, LIMITS } from '../src/index.js';

const fixture = (width = 320, height = 240, background = '#ab8b75') => sharp({ create: { width, height, channels: 3, background } }).png().toBuffer();

test('keeps original bytes; creates deterministic metadata-free web and device JPEG', async () => {
  const source = await fixture();
  const first = await renderPhoto(source, { presetId: 'film', seed: 72 });
  const second = await renderPhoto(source, { presetId: 'film', seed: 72 });
  assert.deepEqual(first.original, source);
  assert.deepEqual(first.web, second.web);
  assert.deepEqual(first.device, second.device);
  const web = await sharp(first.web).metadata(), device = await sharp(first.device).metadata();
  assert.equal(web.format, 'jpeg'); assert.equal(web.exif, undefined); assert.equal(web.icc, undefined);
  assert.equal(device.format, 'jpeg'); assert.equal(device.width, 320); assert.equal(device.height, 240);
  assert.equal(device.isProgressive, false); assert.ok(first.device.length <= LIMITS.deviceBytes);
  assert.equal(first.metadata.presetId, 'film'); assert.equal(first.metadata.sha256.length, 64);
});

test('rotates EXIF-oriented sources and strips personal metadata on both delivery paths', async () => {
  const source = await sharp(await fixture(80, 40)).withMetadata({ orientation: 6, exif: { IFD0: { Artist: 'private-person' } } }).jpeg().toBuffer();
  const result = await renderPhoto(source);
  assert.deepEqual(result.original, source);
  assert.equal(result.metadata.width, 40); assert.equal(result.metadata.height, 80);
  assert.equal((await sharp(result.web).metadata()).exif, undefined);
  const reference = await prepareReference(source);
  assert.equal(reference.width, 40); assert.equal(reference.height, 80);
  assert.equal((await sharp(reference.bytes).metadata()).exif, undefined);
  assert.deepEqual(reference.warnings, ['LOW_RESOLUTION_REFERENCE']);
});

test('portrait device output letterboxes rather than cropping faces or changing aspect ratio', async () => {
  const result = await renderPhoto(await fixture(480, 640, '#d14343'));
  const { data, info } = await sharp(result.device).raw().toBuffer({ resolveWithObject: true });
  assert.equal(info.width, 320); assert.equal(info.height, 240);
  const pixel = (x, y) => [...data.subarray((y * info.width + x) * 3, (y * info.width + x) * 3 + 3)];
  assert.ok(pixel(1, 120).every(value => value > 235), 'paper margin');
  assert.ok(pixel(160, 120)[0] - pixel(160, 120)[1] > 80, 'uncropped center');
});

test('does not upscale tiny capture sources; device canvas remains 320×240', async () => {
  const result = await renderPhoto(await fixture(40, 20));
  assert.equal(result.metadata.width, 40); assert.equal(result.metadata.height, 20);
  const device = await sharp(result.device).metadata();
  assert.equal(device.width, 320); assert.equal(device.height, 240);
  assert.equal((await prepareReference(await fixture(320, 240))).width, 320);
});

test('generated derivatives carry a visible label; provider original remains byte-exact', async () => {
  const source = await fixture(640, 480, '#555555');
  const raw = await renderPhoto(source), labelled = await renderPhoto(source, { aiGenerated: true });
  assert.deepEqual(labelled.original, source);
  assert.notDeepEqual(labelled.web, raw.web); assert.notDeepEqual(labelled.device, raw.device);
  const { data, info } = await sharp(labelled.device).raw().toBuffer({ resolveWithObject: true });
  const corner = (230 * info.width + 286) * 3;
  assert.ok(data[corner] > 120, 'light AI disclosure background is visible on the card');
  const letterI = (226 * info.width + 307) * 3;
  assert.ok(data[letterI] < 115, 'AI vector lettering remains visible without host fonts');
});

test('bounds source size, verifies actual decoding, rejects SVG and animated images', async () => {
  await assert.rejects(renderPhoto(Buffer.alloc(LIMITS.inputBytes + 1)), { code: 'INPUT_TOO_LARGE' });
  await assert.rejects(renderPhoto(Buffer.from('<svg/>')), { code: 'UNSUPPORTED_IMAGE' });
  await assert.rejects(renderPhoto(Buffer.from([255, 216, 255, 0])), { code: 'INVALID_IMAGE' });
  await assert.rejects(renderPhoto(await fixture(), { intensity: 2 }), { code: 'INVALID_OPTIONS' });
  await assert.rejects(renderPhoto(await fixture(), { webMaxEdge: 9000 }), { code: 'INVALID_OPTIONS' });
  await assert.rejects(renderPhoto(await fixture(), { presetId: 'unknown' }), RangeError);
  // A two-page raw image encoded as animated WebP is not silently treated as a still.
  const animated = await sharp(Buffer.alloc(4 * 4 * 3 * 2, 128), { raw: { width: 4, height: 8, channels: 3, pageHeight: 4 } }).webp({ loop: 0, delay: [100, 100] }).toBuffer();
  if ((await sharp(animated).metadata()).pages > 1) await assert.rejects(renderPhoto(animated), { code: 'UNSUPPORTED_IMAGE' });
});

test('non-generative pair is honestly labelled and handles two independent photos', async () => {
  const pair = await createPhotoPair([await fixture(500, 300, '#ff7777'), await fixture(300, 500, '#7788ff')]);
  assert.equal(pair.aiGenerated, false); assert.equal(pair.kind, 'photo-pair');
  assert.equal(pair.metadata.width, 1024); assert.equal(pair.metadata.height, 768);
  await assert.rejects(createPhotoPair([]), { code: 'INVALID_OPTIONS' });
});

test('RGB565 unpacking is endian-explicit and obeys primary colors', () => {
  assert.deepEqual([...rgb565ToRgba(new Uint8Array([0x00, 0xf8, 0xe0, 0x07, 0x1f, 0x00]), 3, 1, 'le')], [255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255]);
  assert.deepEqual([...rgb565ToRgba(new Uint8Array([0xf8, 0x00]), 1, 1, 'be')], [255, 0, 0, 255]);
  assert.throws(() => rgb565ToRgba(new Uint8Array(2), 1, 1), { code: 'INVALID_OPTIONS' });
  assert.throws(() => rgb565ToRgba(new Uint8Array(1), 1, 1, 'le'), { code: 'INVALID_IMAGE' });
});
