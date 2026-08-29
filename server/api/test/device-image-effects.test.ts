import test from 'node:test'
import assert from 'node:assert/strict'
import { deflateSync } from 'node:zlib'
import { renderPhoto } from '@pvc/effects'
import { buildApp } from '../src/app.js'
import { DemoStore, type Photo } from '../src/demo-store.js'

// Synthetic color chart, encoded by the effects module's Sharp pipeline. No files or network.
function colorChartPng(width: number, height: number): Buffer {
  const chunk = (type: string, data: Buffer) => {
    const content = Buffer.concat([Buffer.from(type), data])
    let crc = 0xffffffff
    for (const byte of content) { crc ^= byte; for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0) }
    const size = Buffer.alloc(4); size.writeUInt32BE(data.length)
    const checksum = Buffer.alloc(4); checksum.writeUInt32BE((crc ^ 0xffffffff) >>> 0)
    return Buffer.concat([size, content, checksum])
  }
  const header = Buffer.alloc(13)
  header.writeUInt32BE(width); header.writeUInt32BE(height, 4); header[8] = 8; header[9] = 2
  const pixels = Buffer.alloc(height * (1 + width * 3))
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    const offset = y * (1 + width * 3) + 1 + x * 3
    pixels[offset] = 70 + Math.floor(x * 130 / width)
    pixels[offset + 1] = 90 + Math.floor(y * 110 / height)
    pixels[offset + 2] = 160 - Math.floor(x * 60 / width)
  }
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', header), chunk('IDAT', deflateSync(pixels)), chunk('IEND', Buffer.alloc(0))])
}

// Read a JPEG SOF header without an undeclared Sharp dependency in the API package.
function jpegFrame(bytes: Buffer) {
  assert.equal(bytes.readUInt16BE(0), 0xffd8, 'JPEG SOI marker')
  let offset = 2
  while (offset + 4 <= bytes.length) {
    assert.equal(bytes[offset], 0xff, 'valid JPEG marker boundary')
    while (bytes[offset + 1] === 0xff) offset++
    const marker = bytes[offset + 1]
    if (marker === 0xda || marker === 0xd9) break
    const length = bytes.readUInt16BE(offset + 2)
    assert.ok(length >= 2 && offset + 2 + length <= bytes.length, 'valid JPEG segment length')
    if (marker >= 0xc0 && marker <= 0xcf && ![0xc4, 0xc8, 0xcc].includes(marker)) {
      return { marker, precision: bytes[offset + 4], height: bytes.readUInt16BE(offset + 5), width: bytes.readUInt16BE(offset + 7) }
    }
    offset += 2 + length
  }
  throw new Error('JPEG frame header missing')
}

const webAuth = { authorization: 'Bearer demo-token' }
const ownerDeviceAuth = { authorization: 'Bearer test-owner-device-token' }
const friendAuth = { authorization: 'Bearer demo-user-2' }
const friendDeviceAuth = { authorization: 'Bearer test-friend-device-token' }
const renderedFixture = renderPhoto(colorChartPng(640, 480), { presetId: 'warm', intensity: 0.6, seed: 17 })

async function fixture() {
  const rendered = await renderedFixture
  const store = new DemoStore()
  store.photos.clear()
  const photo: Photo = {
    id: 'device-photo', authorId: 'u_demo_1', filterId: 'warm', caption: '颜色测试',
    width: rendered.metadata.width, height: rendered.metadata.height,
    createdAt: new Date().toISOString(), original: rendered.original, processed: rendered.web,
  }
  store.photos.set(photo.id, photo)
  store.photos.set('private-draft', { ...photo, id: 'private-draft', draftJobId: 'private-job', aiGenerated: true })
  store.devices.set('owner-card', { userId: 'u_demo_1', token: 'test-owner-device-token' })
  store.devices.set('friend-card', { userId: 'u_demo_2', token: 'test-friend-device-token' })
  let reads = 0
  const app = await buildApp({
    store,
    photoStorage: {
      async save() { throw new Error('GET must not write images') },
      async read(photo) { reads++; return photo.processed },
      async remove() { throw new Error('GET must not delete images') },
    },
  })
  return { app, photo, reads: () => reads }
}

function assertDeviceJpeg(bytes: Buffer) {
  assert.deepEqual(jpegFrame(bytes), { marker: 0xc0, precision: 8, width: 320, height: 240 })
  assert.ok(bytes.length <= 100 * 1024, 'device JPEG must fit 100 KiB')
}

test('size=320 returns a baseline 320x240 JPEG within the device byte budget', async () => {
  const f = await fixture()
  try {
    const response = await f.app.inject({ method: 'GET', url: '/v1/photos/device-photo/image?size=320', headers: webAuth })
    assert.equal(response.statusCode, 200)
    assert.equal(response.headers['content-type'], 'image/jpeg')
    assert.equal(response.headers['cache-control'], 'private, no-store')
    assert.equal(response.headers.vary, 'Authorization')
    assertDeviceJpeg(response.rawPayload)
  } finally { await f.app.close() }
})

test('a bound device token selects the same device derivative without a size query', async () => {
  const f = await fixture()
  try {
    const explicit = await f.app.inject({ method: 'GET', url: '/v1/photos/device-photo/image?size=320', headers: webAuth })
    const implicit = await f.app.inject({ method: 'GET', url: '/v1/photos/device-photo/image', headers: ownerDeviceAuth })
    assert.equal(implicit.statusCode, 200)
    assertDeviceJpeg(implicit.rawPayload)
    assert.deepEqual(implicit.rawPayload, explicit.rawPayload)
    assert.equal(implicit.headers['cache-control'], 'private, no-store')
  } finally { await f.app.close() }
})

test('a Web request without size keeps the full stored Web JPEG byte-exact', async () => {
  const f = await fixture()
  try {
    const response = await f.app.inject({ method: 'GET', url: '/v1/photos/device-photo/image', headers: webAuth })
    assert.equal(response.statusCode, 200)
    assert.deepEqual(response.rawPayload, f.photo.processed)
    assert.deepEqual(jpegFrame(response.rawPayload), { marker: 0xc0, precision: 8, width: 640, height: 480 })
    assert.equal(response.headers.vary, 'Authorization')
  } finally { await f.app.close() }
})

test('device resizing does not apply the saved photo filter a second time', async () => {
  const f = await fixture()
  try {
    const expected = await renderPhoto(f.photo.processed, { presetId: 'none', intensity: 0 })
    const doubled = await renderPhoto(f.photo.processed, { presetId: 'warm', intensity: 0.6 })
    assert.notDeepEqual(expected.device, doubled.device, 'fixture detects a second filter application')
    const response = await f.app.inject({ method: 'GET', url: '/v1/photos/device-photo/image?size=320', headers: webAuth })
    assert.equal(response.statusCode, 200)
    assert.deepEqual(response.rawPayload, expected.device, 'same encoded pixels as neutral resizing, not another look')
  } finally { await f.app.close() }
})

test('private drafts stay forbidden to other users and their bound devices at either size', async () => {
  const f = await fixture()
  try {
    for (const headers of [friendAuth, friendDeviceAuth]) for (const query of ['', '?size=320']) {
      const response = await f.app.inject({ method: 'GET', url: `/v1/photos/private-draft/image${query}`, headers })
      assert.equal(response.statusCode, 403)
      assert.equal(response.json().error.code, 'FORBIDDEN')
    }
    assert.equal(f.reads(), 0, 'authorization runs before image reads or conversion')
  } finally { await f.app.close() }
})

test('unsupported sizes are rejected before reading any image for Web and device tokens', async () => {
  const f = await fixture()
  try {
    for (const headers of [webAuth, ownerDeviceAuth]) {
      const response = await f.app.inject({ method: 'GET', url: '/v1/photos/device-photo/image?size=999', headers })
      assert.equal(response.statusCode, 400)
      assert.equal(response.json().error.code, 'BAD_REQUEST')
    }
    assert.equal(f.reads(), 0)
  } finally { await f.app.close() }
})
