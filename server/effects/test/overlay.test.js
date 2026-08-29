import assert from 'node:assert/strict';
import test from 'node:test';
import { createHash } from 'node:crypto';
import { mkdtemp, readFile, writeFile, readdir, rm } from 'node:fs/promises';
import path from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import sharp from 'sharp';
import { createOverlayAsset, exportOverlay, OVERLAY_LIMITS } from '../scripts/export-overlay.mjs';

const hardware = fileURLToPath(new URL('../hardware/', import.meta.url));
const cli = fileURLToPath(new URL('../scripts/export-overlay.mjs', import.meta.url));
// Numeric RGBA fixture, not a photograph or a creative image asset.
const pixels = Buffer.from([255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 255, 255, 255, 17]);
const fixture = () => sharp(pixels, { raw: { width: 2, height: 2, channels: 4 } }).png().toBuffer();
async function temporary(fn) {
  const directory = await mkdtemp(path.join(tmpdir(), 'presence-overlay-test-'));
  try { return await fn(directory); }
  finally { await rm(directory, { recursive: true, force: true }); }
}

test('PNG exports exact primary RGB565 words and separate straight alpha, deterministically', async () => {
  const input = await fixture();
  const first = await createOverlayAsset(input, { symbol: 'tiny_test', sourceName: '/private/input/test.png' });
  const second = await createOverlayAsset(input, { symbol: 'tiny_test', sourceName: '/private/input/test.png' });
  assert.equal(first.width, 2); assert.equal(first.height, 2);
  assert.deepEqual([...first.rgb565], [0xf800, 0x07e0, 0x001f, 0xffff]);
  assert.deepEqual([...first.alpha], [255, 128, 0, 17]);
  assert.equal(first.header, second.header);
  assert.deepEqual(first.provenance, second.provenance);
  assert.equal(first.provenance.sourceSha256, createHash('sha256').update(input).digest('hex'));
  assert.equal(first.provenance.sourceFile, 'test.png');
  assert.equal(first.provenance.license, 'not-inferred');
  assert.equal(first.provenance.payloadBytes, 12);
  assert.doesNotMatch(first.header, /\/private\//);
  assert.match(first.header, /static const uint16_t tiny_test_rgb565\[4\]/);
  assert.match(first.header, /static const uint8_t tiny_test_alpha\[4\]/);
  assert.match(first.header, /NOT linear-light/);
  assert.match(first.header, /byte order is chosen by firmware/);
});

test('auto-orients PNG, strips personal metadata and shrinks to 128 without enlarging', async () => {
  const oriented = await sharp(pixels.subarray(0, 8), { raw: { width: 2, height: 1, channels: 4 } })
    .withMetadata({ orientation: 6, exif: { IFD0: { Artist: 'private-test-artist' } } }).png().toBuffer();
  const result = await createOverlayAsset(oriented, { symbol: 'portrait' });
  assert.equal(result.width, 1); assert.equal(result.height, 2);
  assert.deepEqual([...result.rgb565], [0xf800, 0x07e0]);
  assert.deepEqual([...result.alpha], [255, 128]);
  assert.doesNotMatch(result.header + JSON.stringify(result.provenance), /private-test-artist/);
  const wide = await sharp({ create: { width: 256, height: 32, channels: 4, background: { r: 80, g: 120, b: 180, alpha: 0.5 } } }).png().toBuffer();
  const small = await createOverlayAsset(wide, { symbol: 'small' });
  assert.equal(small.width, 128); assert.equal(small.height, 16);
  assert.ok(small.rgb565.length <= 128 * 128);
  assert.ok(small.alpha.every(value => value === 128));
});

test('rejects malformed, oversized, non-PNG and animated input before export', async () => {
  const png = await fixture();
  for (const invalid of [new Uint8Array(), Buffer.from('<svg/>'), Buffer.from([255, 216, 255]), png.subarray(0, -1), Buffer.concat([png, Buffer.from('extra')])]) {
    await assert.rejects(createOverlayAsset(invalid, { symbol: 'valid' }), /PNG/);
  }
  await assert.rejects(createOverlayAsset(null, { symbol: 'valid' }), /bytes/);
  await assert.rejects(createOverlayAsset(Buffer.alloc(OVERLAY_LIMITS.inputBytes + 1), { symbol: 'valid' }), /10 MiB/);
  const bomb = Buffer.from(png);
  bomb.writeUInt32BE(4001, 16); bomb.writeUInt32BE(4001, 20);
  await assert.rejects(createOverlayAsset(bomb, { symbol: 'valid' }), /16 megapixels/);
  const chunkBomb = Buffer.from(png); chunkBomb.writeUInt32BE(0xffffffff, 8);
  await assert.rejects(createOverlayAsset(chunkBomb, { symbol: 'valid' }), /truncated PNG/);
  const animation = Buffer.alloc(20);
  animation.writeUInt32BE(8, 0); animation.write('acTL', 4, 'ascii'); animation.writeUInt32BE(2, 8);
  await assert.rejects(createOverlayAsset(Buffer.concat([png.subarray(0, 33), animation, png.subarray(33)]), { symbol: 'valid' }), /Animated PNG/);
  const invalidData = Buffer.from(png);
  for (let offset = 8; offset < invalidData.length;) {
    const length = invalidData.readUInt32BE(offset);
    if (invalidData.toString('ascii', offset + 4, offset + 8) === 'IDAT') { invalidData.fill(0, offset + 8, offset + 8 + length); break; }
    offset += length + 12;
  }
  await assert.rejects(createOverlayAsset(invalidData, { symbol: 'valid' }), /cannot be decoded/);
});

test('header identifiers cannot inject paths/code or use C/C++ reserved names', async () => {
  const png = await fixture();
  for (const symbol of ['../escape', 'x;bad()', '_reserved', 'bad-name', '1bad', 'for', 'class', 'namespace', 'x__reserved', 'a'.repeat(65), '', null]) {
    await assert.rejects(createOverlayAsset(png, { symbol }), /C\/C\+\+ identifier/);
  }
});

test('CLI writes header and hash/provenance JSON, refuses overwrite and preserves existing files', async () => temporary(async directory => {
  const input = path.join(directory, 'owned.png'), out = path.join(directory, 'export');
  await writeFile(input, await fixture());
  const args = [cli, '--input', input, '--out', out, '--symbol', 'owned_sticker'];
  const first = spawnSync(process.execPath, args, { encoding: 'utf8', timeout: 15000 });
  assert.equal(first.status, 0, first.stderr);
  const result = JSON.parse(first.stdout);
  assert.equal(result.width, 2); assert.equal(result.height, 2); assert.equal(result.payloadBytes, 12);
  assert.deepEqual((await readdir(out)).sort(), ['owned_sticker.h', 'owned_sticker.json']);
  const header = await readFile(result.header), manifest = await readFile(result.provenance);
  assert.equal(JSON.parse(manifest).license, 'not-inferred');
  const repeat = spawnSync(process.execPath, args, { encoding: 'utf8', timeout: 15000 });
  assert.notEqual(repeat.status, 0); assert.match(repeat.stderr, /EEXIST/);
  assert.deepEqual(await readFile(result.header), header);
  assert.deepEqual(await readFile(result.provenance), manifest);
  // A pre-existing manifest also prevents a half-written/new header from remaining.
  await writeFile(path.join(out, 'blocked.json'), 'existing metadata');
  await assert.rejects(exportOverlay({ input, out, symbol: 'blocked' }), { code: 'EEXIST' });
  assert.equal(await readFile(path.join(out, 'blocked.json'), 'utf8'), 'existing metadata');
  await assert.rejects(readFile(path.join(out, 'blocked.h')), { code: 'ENOENT' });
}));

test('CLI input read is bounded and invalid files do not create output', async () => temporary(async directory => {
  const input = path.join(directory, 'large.png'), out = path.join(directory, 'absent');
  await writeFile(input, Buffer.alloc(OVERLAY_LIMITS.inputBytes + 1));
  await assert.rejects(exportOverlay({ input, out, symbol: 'large' }), /10 MiB/);
  await assert.rejects(readdir(out), { code: 'ENOENT' });
  await assert.rejects(exportOverlay({ input: directory, out, symbol: 'directory' }), /regular file/);
  const usage = spawnSync(process.execPath, [cli], { encoding: 'utf8', timeout: 15000 });
  assert.notEqual(usage.status, 0); assert.match(usage.stderr, /Usage:/);
}));

test('runtime has no heap calls, floating-point work, wire-byte casts or platform SDK dependency', async () => {
  const header = await readFile(path.join(hardware, 'presence_overlay.h'), 'utf8');
  assert.doesNotMatch(header, /\b(?:malloc|calloc|realloc|free|alloca)\s*\(/);
  assert.doesNotMatch(header, /\b(?:float|double)\s+\w/);
  assert.doesNotMatch(header, /#include\s*[<"](?:Arduino|esp_|M5|stdlib)/);
  assert.match(header, /const uint16_t \*overlay/);
  assert.match(header, /const uint8_t \*alpha/);
  assert.match(header, /NOT linear-light/);
  assert.match(header, /INT32_MIN/);
});

function blend(background, foreground, alpha) {
  const mix = (shift, mask) => Math.floor((((foreground >>> shift) & mask) * alpha + ((background >>> shift) & mask) * (255 - alpha) + 127) / 255);
  return (mix(11, 31) << 11) | (mix(5, 63) << 5) | mix(0, 31);
}
function reference(x, y) {
  const frame = Array.from({ length: 12 }, (_, i) => (i * 1237 + 31) & 65535);
  const foreground = [0xf800, 0x07e0, 0x001f, 0xffff], alpha = [255, 128, 0, 17];
  for (let sy = 0; sy < 2; sy++) for (let sx = 0; sx < 2; sx++) {
    const dx = x + sx, dy = y + sy;
    if (dx >= 0 && dy >= 0 && dx < 4 && dy < 3) frame[dy * 4 + dx] = blend(frame[dy * 4 + dx], foreground[sy * 2 + sx], alpha[sy * 2 + sx]);
  }
  return frame;
}

for (const [compiler, language] of [['cc', 'c99'], ['c++', 'c++11']]) {
  const available = spawnSync(compiler, ['--version'], { encoding: 'utf8', timeout: 10000 }).status === 0;
  test(`heap-free overlay compiles and runs as ${language}: clipping, alpha, bounds and byte order`, { skip: !available }, async () => temporary(async directory => {
    const asset = await createOverlayAsset(await fixture(), { symbol: 'tiny_test' });
    await writeFile(path.join(directory, 'asset.h'), asset.header);
    const positions = [[0, 0], [1, 1], [-1, -1], [-1, 1], [1, -1], [3, 2], [4, 0], [0, 3], [-2, 0], [0, -2], [2147483647, 0], [-2147483648, 0], [0, 2147483647], [0, -2147483648]];
    const clipping = positions.map(([x, y]) => `{
      uint16_t guarded[14] = {0xa55au, ${reference(4, 3).join(', ')}, 0x5aa5u};
      const uint16_t expected[12] = {${reference(x, y).join(', ')}};
      assert(presence_overlay_apply_rgb565(guarded + 1, 12, 4, 3, tiny_test_rgb565, 4, tiny_test_alpha, 4, 2, 2, ${x === -2147483648 ? 'INT32_MIN' : x}, ${y === -2147483648 ? 'INT32_MIN' : y}) == 1);
      assert(guarded[0] == 0xa55au && guarded[13] == 0x5aa5u);
      for (size_t i = 0; i < 12; ++i) assert(guarded[i + 1] == expected[i]);
    }`).join('\n');
    const program = `
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "presence_overlay.h"
#include "asset.h"
static uint16_t maximum_frame[640u * 480u];
static const uint16_t maximum_overlay[128u * 128u] = {0xf800u};
static const uint8_t maximum_alpha[128u * 128u] = {255u};
int main(void) {
  assert(tiny_test_width == 2 && tiny_test_height == 2);
  assert(tiny_test_rgb565_words == 4 && tiny_test_alpha_bytes == 4);
  assert(presence_overlay_blend_rgb565(0x001fu, 0xf800u, 128) == 0x800fu);
  uint32_t hash = 2166136261u;
  const uint8_t alphas[] = {1, 17, 127, 128, 254};
  for (uint32_t word = 0; word <= 65535u; ++word) {
    const uint16_t source = (uint16_t)(word * 40503u + 1357u);
    assert(presence_overlay_blend_rgb565((uint16_t)word, source, 0) == word);
    assert(presence_overlay_blend_rgb565((uint16_t)word, source, 255) == source);
    for (size_t i = 0; i < sizeof(alphas); ++i) hash = (hash ^ presence_overlay_blend_rgb565((uint16_t)word, source, alphas[i])) * 16777619u;
  }
  ${clipping}
  uint16_t destination[1] = {0x1234u};
  const uint16_t red[1] = {0xf800u};
  const uint8_t opaque[1] = {255u};
#define INVALID(f, fl, fw, fh, s, sl, a, al, sw, sh) assert(presence_overlay_apply_rgb565(f, fl, fw, fh, s, sl, a, al, sw, sh, 0, 0) == 0)
  INVALID(NULL, 1, 1, 1, red, 1, opaque, 1, 1, 1);
  INVALID(destination, 1, 1, 1, NULL, 1, opaque, 1, 1, 1);
  INVALID(destination, 1, 1, 1, red, 1, NULL, 1, 1, 1);
  INVALID(destination, 0, 1, 1, red, 1, opaque, 1, 1, 1);
  INVALID(destination, 2, 1, 1, red, 1, opaque, 1, 1, 1);
  INVALID(destination, 1, 0, 1, red, 1, opaque, 1, 1, 1);
  INVALID(destination, 1, 1, 0, red, 1, opaque, 1, 1, 1);
  INVALID(destination, 1, 641, 1, red, 1, opaque, 1, 1, 1);
  INVALID(destination, 1, 1, 481, red, 1, opaque, 1, 1, 1);
  INVALID(destination, 1, SIZE_MAX, SIZE_MAX, red, 1, opaque, 1, 1, 1);
  INVALID(destination, 1, 1, 1, red, 0, opaque, 1, 1, 1);
  INVALID(destination, 1, 1, 1, red, 2, opaque, 1, 1, 1);
  INVALID(destination, 1, 1, 1, red, 1, opaque, 0, 1, 1);
  INVALID(destination, 1, 1, 1, red, 1, opaque, 2, 1, 1);
  INVALID(destination, 1, 1, 1, red, 1, opaque, 1, 0, 1);
  INVALID(destination, 1, 1, 1, red, 1, opaque, 1, 1, 0);
  INVALID(destination, 1, 1, 1, red, 1, opaque, 1, 129, 1);
  INVALID(destination, 1, 1, 1, red, 1, opaque, 1, 1, 129);
  INVALID(destination, 1, 1, 1, red, 1, opaque, 1, SIZE_MAX, SIZE_MAX);
  assert(destination[0] == 0x1234u);
#undef INVALID
  assert(presence_overlay_apply_rgb565(maximum_frame, 640u * 480u, 640, 480, maximum_overlay, 128u * 128u, maximum_alpha, 128u * 128u, 128, 128, 639, 479) == 1);
  assert(maximum_frame[640u * 480u - 1] == 0xf800u);
  const uint8_t little_endian[2] = {0x00, 0xf8}, big_endian[2] = {0xf8, 0x00};
  const uint16_t from_le = (uint16_t)(little_endian[0] | ((uint16_t)little_endian[1] << 8));
  const uint16_t from_be = (uint16_t)(((uint16_t)big_endian[0] << 8) | big_endian[1]);
  assert(from_le == 0xf800u && from_be == from_le);
  assert(presence_overlay_apply_rgb565(destination, 1, 1, 1, &from_be, 1, opaque, 1, 1, 1, 0, 0) == 1);
  assert(destination[0] == 0xf800u && (destination[0] >> 8) == 0xf8u && (destination[0] & 255u) == 0u);
  printf("%u\\n", (unsigned)hash);
  return 0;
}
`;
    const source = path.join(directory, language === 'c99' ? 'runner.c' : 'runner.cpp'), executable = path.join(directory, 'runner');
    await writeFile(source, program);
    const compilation = spawnSync(compiler, [`-std=${language}`, '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I', hardware, source, '-o', executable], { encoding: 'utf8', timeout: 20000 });
    assert.equal(compilation.status, 0, compilation.stderr || compilation.error?.message);
    const execution = spawnSync(executable, [], { encoding: 'utf8', timeout: 15000 });
    assert.equal(execution.status, 0, execution.stderr || execution.error?.message);
    let hash = 2166136261;
    for (let word = 0; word <= 65535; word++) for (const alpha of [1, 17, 127, 128, 254]) hash = Math.imul(hash ^ blend(word, (word * 40503 + 1357) & 65535, alpha), 16777619) >>> 0;
    assert.equal(execution.stdout.trim(), String(hash));
  }));
}
