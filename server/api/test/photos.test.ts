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
    'x-width': '320',
    'x-height': '240',
  }
  const first = await app.inject({ method: 'POST', url: '/v1/photos', headers, payload: jpeg })
  assert.equal(first.statusCode, 201)
  const retry = await app.inject({ method: 'POST', url: '/v1/photos', headers, payload: jpeg })
  assert.equal(retry.statusCode, 200)
  assert.equal(retry.json().photo_id, first.json().photo_id)
  const image = await app.inject({ method: 'GET', url: `/v1/photos/${first.json().photo_id}/image` })
  assert.equal(image.statusCode, 200)
  assert.equal(image.headers['content-type'], 'image/jpeg')
  const feed = await app.inject({ method: 'GET', url: '/v1/feed', headers: { authorization: 'Bearer demo-token' } })
  const uploaded = feed.json().items.find((item: any) => item.photo_id === first.json().photo_id)
  assert.equal(uploaded.play_type, 'template')
  assert.equal(uploaded.beauty, 42)
  assert.equal(uploaded.sticker, 'star')
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
