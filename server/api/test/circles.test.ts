import test from 'node:test'
import assert from 'node:assert/strict'
import { buildApp } from '../src/app.js'

const jpeg = Buffer.from([0xff, 0xd8, 0xff, 0xd9])
const auth = { authorization: 'Bearer demo-token' }

test('lists seeded circles with joined state and counts', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/pvc-circles-list' })
  const res = await app.inject({ method: 'GET', url: '/v1/circles', headers: auth })
  assert.equal(res.statusCode, 200)
  const items = res.json().items
  assert.equal(items[0].id, 'c_small')
  const sky = items.find((c: any) => c.id === 'c_sky')
  assert.ok(sky)
  assert.equal(sky.type, 'big')
  assert.equal(sky.joined, true)
  assert.ok(sky.photo_count >= 3)
  const desk = items.find((c: any) => c.id === 'c_desk')
  assert.equal(desk.joined, false)
  await app.close()
})

test('join and leave a big circle changes subscription state', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/pvc-circles-join' })
  const before = await app.inject({ method: 'GET', url: '/v1/circles', headers: auth })
  const desk = before.json().items.find((c: any) => c.id === 'c_desk')
  assert.equal(desk.joined, false)
  const joined = await app.inject({ method: 'POST', url: '/v1/circles/c_desk/join', headers: auth })
  assert.equal(joined.statusCode, 200)
  assert.equal(joined.json().joined, true)
  const left = await app.inject({ method: 'POST', url: '/v1/circles/c_desk/leave', headers: auth })
  assert.equal(left.statusCode, 204)
  const after = await app.inject({ method: 'GET', url: '/v1/circles', headers: auth })
  assert.equal(after.json().items.find((c: any) => c.id === 'c_desk').joined, false)
  await app.close()
})

test('circle feed requires membership and serves curated photos', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/pvc-circles-feed' })
  const forbidden = await app.inject({ method: 'GET', url: '/v1/circles/c_desk/feed', headers: auth })
  assert.equal(forbidden.statusCode, 403)
  const missing = await app.inject({ method: 'GET', url: '/v1/circles/c_nope/feed', headers: auth })
  assert.equal(missing.statusCode, 404)
  const sky = await app.inject({ method: 'GET', url: '/v1/circles/c_sky/feed', headers: auth })
  assert.equal(sky.statusCode, 200)
  assert.ok(sky.json().items.length >= 3)
  assert.ok(sky.json().items.every((i: any) => i.circle === '傍晚的天空'))
  const small = await app.inject({ method: 'GET', url: '/v1/circles/c_small/feed', headers: auth })
  assert.equal(small.statusCode, 200)
  assert.ok(small.json().items.every((i: any) => !i.circle_id))
  await app.close()
})

test('posting to a circle requires membership and stamps circle id', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/pvc-circles-post' })
  const headers = { ...auth, 'content-type': 'image/jpeg', 'idempotency-key': 'k-circles-1', 'x-circle-id': 'c_desk' }
  const forbidden = await app.inject({ method: 'POST', url: '/v1/photos', headers, payload: jpeg })
  assert.equal(forbidden.statusCode, 403)
  await app.inject({ method: 'POST', url: '/v1/circles/c_desk/join', headers: auth })
  const ok = await app.inject({ method: 'POST', url: '/v1/photos', headers: { ...headers, 'idempotency-key': 'k-circles-2' }, payload: jpeg })
  assert.equal(ok.statusCode, 201)
  const feed = await app.inject({ method: 'GET', url: '/v1/circles/c_desk/feed', headers: auth })
  const mine = feed.json().items.find((i: any) => i.photo_id === ok.json().photo_id)
  assert.ok(mine)
  assert.equal(mine.circle_id, 'c_desk')
  assert.equal(mine.circle, '宿舍窗台')
  await app.close()
})

test('big circle photos are visible to subscribers who are not friends', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/pvc-circles-visibility' })
  const lunaAuth = { authorization: 'Bearer demo-user-3' }
  const curatedIds = ['p_cur_1', 'p_cur_2', 'p_cur_4', 'p_cur_5']
  // luna is not subscribed to any big circle by default, so curated photos hidden from her feed
  const visibleBefore = await app.inject({ method: 'GET', url: '/v1/feed?limit=32', headers: lunaAuth })
  const beforeIds = visibleBefore.json().items.map((i: any) => i.photo_id)
  assert.ok(curatedIds.every(id => !beforeIds.includes(id)))
  // and the image endpoint blocks her too
  const blockedBefore = await app.inject({ method: 'GET', url: '/v1/photos/p_cur_1/image', headers: lunaAuth })
  assert.equal(blockedBefore.statusCode, 403)
  await app.inject({ method: 'POST', url: '/v1/circles/c_sky/join', headers: lunaAuth })
  const visibleAfter = await app.inject({ method: 'GET', url: '/v1/feed?limit=32', headers: lunaAuth })
  const afterIds = visibleAfter.json().items.map((i: any) => i.photo_id)
  assert.ok(afterIds.includes('p_cur_1'))
  assert.ok(afterIds.includes('p_cur_2'))
  // but desk circle photo still hidden
  assert.ok(!afterIds.includes('p_cur_4'))
  // image endpoint enforces the same rule
  const imgForbidden = await app.inject({ method: 'GET', url: '/v1/photos/p_cur_4/image', headers: lunaAuth })
  assert.equal(imgForbidden.statusCode, 403)
  // subscribed curated photo serves its seeded bytes even without an upload on disk
  const imgAllowed = await app.inject({ method: 'GET', url: '/v1/photos/p_cur_1/image', headers: lunaAuth })
  assert.equal(imgAllowed.statusCode, 200)
  assert.equal(imgAllowed.headers['content-type'], 'image/jpeg')
  await app.close()
})

test('feed circle_id filter scopes small circle to ungrouped friend photos', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/pvc-circles-filter' })
  const all = await app.inject({ method: 'GET', url: '/v1/feed?limit=32', headers: auth })
  const allIds = all.json().items.map((i: any) => i.photo_id)
  assert.ok(allIds.includes('p_cur_1'))
  const small = await app.inject({ method: 'GET', url: '/v1/feed?limit=32&circle_id=c_small', headers: auth })
  const smallItems = small.json().items
  assert.ok(smallItems.every((i: any) => i.circle_id === null || i.circle_id === undefined))
  assert.ok(smallItems.length > 0)
  const sky = await app.inject({ method: 'GET', url: '/v1/feed?limit=32&circle_id=c_sky', headers: auth })
  assert.ok(sky.json().items.every((i: any) => i.circle_id === 'c_sky'))
  assert.ok(sky.json().items.length >= 3)
  await app.close()
})

test('device feed mixes curated big-circle photos when friends go quiet', async () => {
  // Fresh friend content exists in seeds (minutes ago), so device feed is friends-only
  const freshApp = await buildApp({ uploadsDir: '/tmp/pvc-circles-device-fresh' })
  const fresh = await freshApp.inject({ method: 'GET', url: '/v1/feed?mode=device&limit=32', headers: auth })
  assert.equal(fresh.statusCode, 200)
  const freshIds = fresh.json().items.map((i: any) => i.photo_id)
  assert.ok(!freshIds.includes('p_cur_1'))
  await freshApp.close()

  // With all seeds older than 24h, the device feed mixes in curated big-circle photos (max 10)
  const quietApp = await buildApp({ uploadsDir: '/tmp/pvc-circles-device-quiet', seedAgeMs: 48 * 3600 * 1000 })
  const res = await quietApp.inject({ method: 'GET', url: '/v1/feed?mode=device&limit=32', headers: auth })
  assert.equal(res.statusCode, 200)
  const ids = res.json().items.map((i: any) => i.photo_id)
  assert.ok(ids.includes('p_cur_1'))
  assert.ok(ids.includes('p_cur_3'))
  // 宿舍窗台 not subscribed by ayan → excluded
  assert.ok(!ids.includes('p_cur_4'))
  // curated photos are capped at 10
  const curatedCount = ids.filter((id: string) => id.startsWith('p_cur_')).length
  assert.ok(curatedCount <= 10)
  await quietApp.close()
})
