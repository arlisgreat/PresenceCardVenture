import test from 'node:test'
import assert from 'node:assert/strict'
import { buildApp } from '../src/app.js'

test('returns the authenticated demo profile with a shareable friend code', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-profile' })
  const response = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: 'Bearer demo-token' } })
  assert.equal(response.statusCode, 200)
  assert.deepEqual(response.json(), { id: 'u_demo_1', username: 'ayan', display_name: '阿岩', friend_code: '100001' })
  await app.close()
})

test('protects the profile endpoint from missing or invalid tokens', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-profile-auth' })
  const missing = await app.inject({ method: 'GET', url: '/v1/me' })
  const invalid = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: 'Bearer invalid' } })
  assert.equal(missing.statusCode, 401)
  assert.equal(invalid.statusCode, 401)
  await app.close()
})
