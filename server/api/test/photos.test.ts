import test from 'node:test'
import assert from 'node:assert/strict'
import { buildApp } from '../src/app.js'

const jpeg = Buffer.from([0xff, 0xd8, 0xff, 0xd9])

test('uploads jpeg, supports idempotent retry, and serves image', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-photos' })
  const headers = {
    authorization: 'Bearer demo-token',
    'content-type': 'image/jpeg',
    'idempotency-key': 'device-1-1',
    'x-filter-id': 'warm',
    'x-play-type': 'template',
    'x-beauty': '42',
    'x-sticker': 'star',
    'x-circle': encodeURIComponent('傍晚的天空'),
    'x-width': '320',
    'x-height': '240',
  }
  const first = await app.inject({ method: 'POST', url: '/v1/photos', headers, payload: jpeg })
  assert.equal(first.statusCode, 201)
  const retry = await app.inject({ method: 'POST', url: '/v1/photos', headers, payload: jpeg })
  assert.equal(retry.statusCode, 200)
  assert.equal(retry.json().photo_id, first.json().photo_id)
  const image = await app.inject({ method: 'GET', url: `/v1/photos/${first.json().photo_id}/image`, headers: { authorization: 'Bearer demo-token' } })
  assert.equal(image.statusCode, 200)
  assert.equal(image.headers['content-type'], 'image/jpeg')
  const feed = await app.inject({ method: 'GET', url: '/v1/feed', headers: { authorization: 'Bearer demo-token' } })
  const uploaded = feed.json().items.find((item: any) => item.photo_id === first.json().photo_id)
  assert.equal(uploaded.play_type, 'template')
  assert.equal(uploaded.beauty, 42)
  assert.equal(uploaded.sticker, 'star')
  assert.equal(uploaded.circle, '傍晚的天空')
  await app.close()
})

test('rejects oversized and non-jpeg uploads', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-photos-2' })
  const base = { authorization: 'Bearer demo-token', 'idempotency-key': 'k', 'x-filter-id': 'none', 'x-width': '1', 'x-height': '1' }
  const badType = await app.inject({ method: 'POST', url: '/v1/photos', headers: { ...base, 'content-type': 'application/octet-stream' }, payload: jpeg })
  assert.equal(badType.statusCode, 415)
  const tooLarge = await app.inject({ method: 'POST', url: '/v1/photos', headers: { ...base, 'content-type': 'image/jpeg' }, payload: Buffer.alloc(1024 * 1024 + 1) })
  assert.equal(tooLarge.statusCode, 413)
  await app.close()
})

test('validates photo metadata without turning malformed headers into server errors', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-photo-metadata' })
  const base = { authorization: 'Bearer demo-token', 'content-type': 'image/jpeg', 'x-filter-id': 'none', 'x-width': '320', 'x-height': '240' }
  const malformedCaption = await app.inject({ method: 'POST', url: '/v1/photos', headers: { ...base, 'idempotency-key': 'metadata-1', 'x-caption': '%E0%A4%A' }, payload: jpeg })
  assert.equal(malformedCaption.statusCode, 400)
  assert.equal(malformedCaption.json().error.code, 'BAD_REQUEST')

  const longCaption = await app.inject({ method: 'POST', url: '/v1/photos', headers: { ...base, 'idempotency-key': 'metadata-2', 'x-caption': encodeURIComponent('a'.repeat(141)) }, payload: jpeg })
  assert.equal(longCaption.statusCode, 400)

  const invalidDimensions = await app.inject({ method: 'POST', url: '/v1/photos', headers: { ...base, 'idempotency-key': 'metadata-3', 'x-width': '0' }, payload: jpeg })
  assert.equal(invalidDimensions.statusCode, 400)
  await app.close()
})

test('routes photo persistence through an injectable storage adapter', async () => {
  const calls: string[] = []
  const storedImage = Buffer.from([0xff, 0xd8, 0x42, 0xff, 0xd9])
  const storage = {
    async save(photo: { id: string }) { calls.push(`save:${photo.id}`) },
    async read(photo: { id: string }) { calls.push(`read:${photo.id}`); return storedImage },
    async remove(photo: { id: string }) { calls.push(`remove:${photo.id}`) },
  }
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-photo-adapter', photoStorage: storage })
  const created = await app.inject({
    method: 'POST',
    url: '/v1/photos',
    headers: { authorization: 'Bearer demo-token', 'content-type': 'image/jpeg', 'idempotency-key': 'adapter-1' },
    payload: jpeg,
  })
  assert.equal(created.statusCode, 201)
  const id = created.json().photo_id
  assert.deepEqual(calls, [`save:${id}`])
  const image = await app.inject({ method: 'GET', url: `/v1/photos/${id}/image`, headers: { authorization: 'Bearer demo-token' } })
  assert.equal(image.statusCode, 200)
  assert.deepEqual(image.rawPayload, storedImage)
  assert.deepEqual(calls, [`save:${id}`, `read:${id}`])
  const removed = await app.inject({ method: 'DELETE', url: `/v1/photos/${id}`, headers: { authorization: 'Bearer demo-token' } })
  assert.equal(removed.statusCode, 204)
  assert.deepEqual(calls, [`save:${id}`, `read:${id}`, `remove:${id}`])
  await app.close()
})

test('does not publish metadata when photo storage fails', async () => {
  const storage = {
    async save() { throw new Error('disk unavailable') },
    async remove() {},
  }
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-photo-storage-failure', photoStorage: storage })
  const before = await app.inject({ method: 'GET', url: '/v1/photos/mine', headers: { authorization: 'Bearer demo-token' } })
  const failed = await app.inject({
    method: 'POST',
    url: '/v1/photos',
    headers: { authorization: 'Bearer demo-token', 'content-type': 'image/jpeg', 'idempotency-key': 'storage-failure-1' },
    payload: jpeg,
  })
  assert.equal(failed.statusCode, 503)
  assert.equal(failed.json().error.code, 'STORAGE_UNAVAILABLE')
  const after = await app.inject({ method: 'GET', url: '/v1/photos/mine', headers: { authorization: 'Bearer demo-token' } })
  assert.equal(after.json().items.length, before.json().items.length)
  await app.close()
})

test('reports read failures without leaking internal storage errors', async () => {
  const storage = {
    async save() {},
    async read() { throw new Error('object store timeout') },
    async remove() {},
  }
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-photo-read-failure', photoStorage: storage })
  const created = await app.inject({
    method: 'POST',
    url: '/v1/photos',
    headers: { authorization: 'Bearer demo-token', 'content-type': 'image/jpeg', 'idempotency-key': 'read-failure-1' },
    payload: jpeg,
  })
  const image = await app.inject({ method: 'GET', url: `/v1/photos/${created.json().photo_id}/image`, headers: { authorization: 'Bearer demo-token' } })
  assert.equal(image.statusCode, 503)
  assert.equal(image.json().error.code, 'STORAGE_UNAVAILABLE')
  await app.close()
})

test('keeps photo metadata when storage deletion fails', async () => {
  const storage = {
    async save() {},
    async read() { return jpeg },
    async remove() { throw new Error('object store timeout') },
  }
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-photo-delete-failure', photoStorage: storage })
  const created = await app.inject({
    method: 'POST',
    url: '/v1/photos',
    headers: { authorization: 'Bearer demo-token', 'content-type': 'image/jpeg', 'idempotency-key': 'delete-failure-1' },
    payload: jpeg,
  })
  const id = created.json().photo_id
  const removed = await app.inject({ method: 'DELETE', url: `/v1/photos/${id}`, headers: { authorization: 'Bearer demo-token' } })
  assert.equal(removed.statusCode, 503)
  assert.equal(removed.json().error.code, 'STORAGE_UNAVAILABLE')
  const mine = await app.inject({ method: 'GET', url: '/v1/photos/mine', headers: { authorization: 'Bearer demo-token' } })
  assert.ok(mine.json().items.some((item: any) => item.photo_id === id))
  await app.close()
})

test('enforces the per-device daily upload limit', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-photos-limit', uploadDailyLimit: 1 })
  const base = { authorization: 'Bearer demo-token', 'content-type': 'image/jpeg', 'x-filter-id': 'none', 'x-width': '1', 'x-height': '1' }
  const first = await app.inject({ method: 'POST', url: '/v1/photos', headers: { ...base, 'idempotency-key': 'limit-1' }, payload: jpeg })
  assert.equal(first.statusCode, 201)
  const limited = await app.inject({ method: 'POST', url: '/v1/photos', headers: { ...base, 'idempotency-key': 'limit-2' }, payload: jpeg })
  assert.equal(limited.statusCode, 429)
  assert.equal(limited.json().error.code, 'RATE_LIMITED')
  assert.ok(limited.json().retry_after > 0)
  await app.close()
})

test('only owner can delete photos', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-photos-3' })
  const headers = { authorization: 'Bearer demo-token', 'content-type': 'image/jpeg', 'idempotency-key': 'delete-k', 'x-filter-id': 'none', 'x-width': '1', 'x-height': '1' }
  const created = await app.inject({ method: 'POST', url: '/v1/photos', headers, payload: jpeg })
  const other = await app.inject({ method: 'DELETE', url: `/v1/photos/${created.json().photo_id}`, headers: { authorization: 'Bearer demo-user-2' } })
  assert.equal(other.statusCode, 403)
  const deleted = await app.inject({ method: 'DELETE', url: `/v1/photos/${created.json().photo_id}`, headers: { authorization: 'Bearer demo-token' } })
  assert.equal(deleted.statusCode, 204)
  await app.close()
})

test('photo downloads require authentication and an authorized relationship', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-photo-visibility' })
  const feed = await app.inject({ method: 'GET', url: '/v1/feed', headers: { authorization: 'Bearer demo-token' } })
  const lunaPhoto = feed.json().items.find((item: any) => item.author.username === 'luna').photo_id

  const anonymous = await app.inject({ method: 'GET', url: `/v1/photos/${lunaPhoto}/image` })
  assert.equal(anonymous.statusCode, 401)

  const unrelated = await app.inject({ method: 'GET', url: `/v1/photos/${lunaPhoto}/image`, headers: { authorization: 'Bearer demo-user-2' } })
  assert.equal(unrelated.statusCode, 403)

  const authorized = await app.inject({ method: 'GET', url: `/v1/photos/${lunaPhoto}/image`, headers: { authorization: 'Bearer demo-token' } })
  assert.equal(authorized.statusCode, 200)
  assert.equal(authorized.headers['content-type'], 'image/jpeg')
  await app.close()
})
