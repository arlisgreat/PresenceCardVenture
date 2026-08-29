import test from 'node:test'
import assert from 'node:assert/strict'
import { buildApp } from '../src/app.js'
import { DemoStore } from '../src/demo-store.js'

test('production AI gate checks canonical routes, including percent-encoded path segments', async () => {
  let paidProviderCalls = 0
  const app = await buildApp({
    requireProductionServices: true,
    aiProvider: { name: 'fake-paid-provider', async generate() { paidProviderCalls++; return { status: 'failed' } } },
  })
  try {
    for (const url of ['/v1/ai/jobs', '/v1/%61i/jobs', '/v1/ai/%6aobs', '/v1/%61%69/%6A%6F%62%73', '/v1/ai/jobs?demo=1']) {
      const response = await app.inject({
        method: 'POST', url,
        headers: { authorization: 'Bearer demo-token', 'content-type': 'application/json' },
        payload: { material_ids: ['p_demo_2', 'p_demo_4'] },
      })
      assert.equal(response.statusCode, 503, url)
      assert.equal(response.json().error.code, 'AI_NOT_READY')
    }
    const guardedRoutes = [
      { method: 'GET', url: '/v1/%61i/jobs/nonexistent' },
      { method: 'POST', url: '/v1/ai/%6aobs/nonexistent/publish' },
      { method: 'DELETE', url: '/v1/%61i/jobs/nonexistent/result' },
    ] as const
    for (const route of guardedRoutes) {
      const response = await app.inject({ ...route, headers: { authorization: 'Bearer demo-token' } })
      assert.equal(response.statusCode, 503, `${route.method} ${route.url}`)
    }
    await new Promise(resolve => setTimeout(resolve, 15))
    assert.equal(paidProviderCalls, 0)
  } finally { await app.close() }
})

test('reaction privacy is determined by the route photo, never an extra body photo_id', async () => {
  const store = new DemoStore()
  store.photos.set('guard-draft', { ...store.photos.get('p_demo_2')!, id: 'guard-draft', draftJobId: 'private-job' })
  const app = await buildApp({ store })
  const headers = { authorization: 'Bearer demo-user-2', 'content-type': 'application/json' }
  try {
    for (const photo_id of [undefined, 'p_demo_1', '', null]) {
      const created = await app.inject({ method: 'POST', url: '/v1/photos/guard-draft/reactions', headers, payload: { type: 'heart', photo_id } })
      assert.equal(created.statusCode, 403)
      const removed = await app.inject({ method: 'DELETE', url: '/v1/photos/guard-draft/reactions/heart', headers, payload: { photo_id } })
      assert.equal(removed.statusCode, 403)
    }
    const encoded = await app.inject({ method: 'POST', url: '/v1/photos/guard-draft/%72eactions', headers, payload: { type: 'heart', photo_id: 'p_demo_1' } })
    assert.equal(encoded.statusCode, 403)
    assert.equal(store.reactions.size, 0)
    const message = await app.inject({ method: 'POST', url: '/v1/messages', headers, payload: { to: 'ayan', photo_id: 'guard-draft' } })
    assert.equal(message.statusCode, 403)
    assert.equal(store.messages.length, 0)
    const visible = await app.inject({ method: 'POST', url: '/v1/photos/p_demo_1/reactions', headers, payload: { type: 'heart', photo_id: 'guard-draft' } })
    assert.equal(visible.statusCode, 201, 'an unrelated body field must not override the route photo')
  } finally { await app.close() }
})
