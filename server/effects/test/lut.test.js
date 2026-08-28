import assert from 'node:assert/strict';
import test from 'node:test';
import { generateCube, generateRgb565Lut, generateRgb565Header } from '../src/lut.js';
import { transformRgb } from '../src/pixels.js';
import { PRESETS } from '../src/presets.js';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

function cubeSamples(cube) {
  return cube.split('\n').filter((line) => /^\d+\.\d+ /.test(line)).map((line) => line.split(' ').map(Number));
}

test('identity cube uses the standard red-fastest 3D sample order', () => {
  const cube = generateCube('none', { size: 2 });
  assert.match(cube, /LUT_3D_SIZE 2/);
  assert.match(cube, /Color only/);
  assert.deepEqual(cubeSamples(cube), [
    [0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 1, 0],
    [0, 0, 1], [1, 0, 1], [0, 1, 1], [1, 1, 1],
  ]);
});

test('cube samples match unquantized global transforms for every look', () => {
  for (const { id } of PRESETS) {
    const size = 5;
    const samples = cubeSamples(generateCube(id, { size, intensity: 0.7 }));
    assert.equal(samples.length, size ** 3);
    for (let b = 0; b < size; b++) {
      for (let g = 0; g < size; g++) {
        for (let r = 0; r < size; r++) {
          const actual = samples[(b * size + g) * size + r];
          const expected = transformRgb([r * 255 / (size - 1), g * 255 / (size - 1), b * 255 / (size - 1)], id, 0.7);
          expected.forEach((channel, i) => assert.ok(Math.abs(actual[i] - channel / 255) <= 0.000000051));
        }
      }
    }
  }
});

test('RGB565 encodes channel bit depths and keeps red-fastest ordering', () => {
  assert.deepEqual(Array.from(generateRgb565Lut('none', { size: 2 })), [0x0000, 0xf800, 0x07e0, 0xffe0, 0x001f, 0xf81f, 0x07ff, 0xffff]);
  const size = 5;
  for (const { id } of PRESETS) {
    const lut = generateRgb565Lut(id, { size });
    assert.ok(lut instanceof Uint16Array);
    assert.equal(lut.length, size ** 3);
    for (const [r, g, b] of [[1, 2, 3], [2, 4, 0], [4, 4, 4]]) {
      const rgb = transformRgb([r * 255 / (size - 1), g * 255 / (size - 1), b * 255 / (size - 1)], id);
      const packed = (Math.round(rgb[0] * 31 / 255) << 11) | (Math.round(rgb[1] * 63 / 255) << 5) | Math.round(rgb[2] * 31 / 255);
      assert.equal(lut[(b * size + g) * size + r], packed);
    }
    assert.deepEqual(generateRgb565Lut(id, { size, intensity: 0 }), generateRgb565Lut('none', { size }));
  }
});

test('default LUT has 17 cubed entries and is byte-for-byte reproducible', () => {
  const first = generateRgb565Lut('film');
  assert.equal(first.length, 17 ** 3);
  assert.equal(first.byteLength, 9826);
  assert.deepEqual(first, generateRgb565Lut('film'));
  assert.equal(generateCube('warm', { size: 3 }), generateCube('warm', { size: 3 }));
});

test('C header is self-contained, explicit about spatial effects/byte order, and sanitizes identifiers', () => {
  const header = generateRgb565Header('warm', { size: 2, symbol: 'card_warm' });
  assert.match(header, /#include <stdint.h>/);
  assert.match(header, /static const uint8_t card_warm_size = 2;/);
  assert.match(header, /static const uint16_t card_warm\[8\]/);
  assert.match(header, /spatial highlight glow are excluded/);
  assert.match(header, /select byte order/);
  assert.match(header, /card_warm_apply_rgb565\(uint16_t pixel\)/);
  assert.match(header, /nearest grid samples/);
  assert.equal((header.match(/0x[0-9a-f]{4}/g) ?? []).length, 8);
  for (const symbol of ['bad-name', 'x;evil()', '1name', '', 'for', '_reserved', null]) {
    assert.throws(() => generateRgb565Header('warm', { symbol }), /C identifier/);
  }
});

test('generated original/zero-strength helpers bypass coarse grid quantization', () => {
  for (const header of [generateRgb565Header('none'), generateRgb565Header('warm', { intensity: 0 })]) {
    assert.match(header, /Preserve original RGB565 exactly/);
    assert.match(header, /return pixel;/);
    assert.doesNotMatch(header, /const uint32_t index/);
  }
});

const ccAvailable = spawnSync('cc', ['--version'], { encoding: 'utf8' }).status === 0;

test('portable C runtime matches JS for all 65536 RGB565 inputs, guards short LUTs and compiles as C++', { skip: !ccAvailable }, () => {
  const directory = mkdtempSync(path.join(tmpdir(), 'presence-lut-test-'));
  try {
    writeFileSync(path.join(directory, 'warm.h'), generateRgb565Header('warm', { symbol: 'tiny_warm' }));
    writeFileSync(path.join(directory, 'none.h'), generateRgb565Header('none', { symbol: 'tiny_none' }));
    const program = `
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "presence_lut.h"
#include "warm.h"
#include "none.h"
int main(void) {
  uint32_t index_hash = 2166136261u;
  uint32_t output_hash = 2166136261u;
  assert(tiny_warm_size == 17u && tiny_none_size == 17u);
  assert(presence_lut17_index_rgb565(0x0000u) == 0u);
  assert(presence_lut17_index_rgb565(0xf800u) == 16u);
  assert(presence_lut17_index_rgb565(0x07e0u) == 272u);
  assert(presence_lut17_index_rgb565(0x001fu) == 4624u);
  assert(presence_lut17_index_rgb565(0xffffu) == 4912u);
  for (uint32_t word = 0; word <= 65535u; word++) {
    const uint16_t pixel = (uint16_t)word;
    const uint16_t index = presence_lut17_index_rgb565(pixel);
    const uint16_t actual = presence_lut17_apply_rgb565(pixel, tiny_warm, 4913u);
    const uint16_t identity = presence_lut17_apply_rgb565(pixel, tiny_none, 4913u);
    assert(index < PRESENCE_LUT17_ENTRIES);
    assert(actual == tiny_warm_apply_rgb565(pixel));
    assert(tiny_none_apply_rgb565(pixel) == pixel);
    assert(presence_lut17_apply_rgb565(pixel, NULL, 4913u) == pixel);
    assert(presence_lut17_apply_rgb565(pixel, tiny_warm, 4912u) == pixel);
    assert(abs((int)((identity >> 11) & 31u) - (int)((pixel >> 11) & 31u)) <= 1);
    assert(abs((int)((identity >> 5) & 63u) - (int)((pixel >> 5) & 63u)) <= 2);
    assert(abs((int)(identity & 31u) - (int)(pixel & 31u)) <= 1);
    index_hash = (index_hash ^ index) * 16777619u;
    output_hash = (output_hash ^ actual) * 16777619u;
  }
  printf("%u %u\\n", (unsigned)index_hash, (unsigned)output_hash);
  return 0;
}
`;
    const source = path.join(directory, 'runner.c');
    const executable = path.join(directory, 'runner');
    writeFileSync(source, program);
    const hardwarePath = fileURLToPath(new URL('../hardware/', import.meta.url));
    const compilation = spawnSync('cc', ['-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-I', hardwarePath, source, '-o', executable], { encoding: 'utf8', timeout: 15000 });
    assert.equal(compilation.status, 0, compilation.stderr || compilation.error?.message);
    const execution = spawnSync(executable, [], { encoding: 'utf8', timeout: 15000 });
    assert.equal(execution.status, 0, execution.stderr || execution.error?.message);
    let indexHash = 2166136261;
    let outputHash = 2166136261;
    const table = generateRgb565Lut('warm');
    for (let pixel = 0; pixel <= 65535; pixel++) {
      const red = Math.round(((pixel >> 11) & 31) * 16 / 31);
      const green = Math.round(((pixel >> 5) & 63) * 16 / 63);
      const blue = Math.round((pixel & 31) * 16 / 31);
      const index = (blue * 17 + green) * 17 + red;
      indexHash = Math.imul(indexHash ^ index, 16777619) >>> 0;
      outputHash = Math.imul(outputHash ^ table[index], 16777619) >>> 0;
    }
    assert.equal(execution.stdout.trim(), `${indexHash} ${outputHash}`);
    const cppCompilation = spawnSync('c++', ['-x', 'c++', '-std=c++11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror', '-I', hardwarePath, source], { encoding: 'utf8', timeout: 15000 });
    assert.equal(cppCompilation.status, 0, cppCompilation.stderr || cppCompilation.error?.message);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test('all exporters validate size, intensity and preset', () => {
  for (const generator of [generateCube, generateRgb565Lut, generateRgb565Header]) {
    assert.throws(() => generator('missing'), /Unknown Presence preset/);
    for (const size of [0, 1, 1.5, 66, NaN, Infinity, '17']) assert.throws(() => generator('none', { size }), /LUT size/);
    for (const intensity of [-1, 1.01, NaN, Infinity, null, '1']) assert.throws(() => generator('none', { intensity }), /intensity/);
  }
});
