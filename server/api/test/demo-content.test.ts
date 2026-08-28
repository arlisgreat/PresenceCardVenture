import assert from 'node:assert/strict'
import test from 'node:test'
import { buildApp } from '../src/app.js'

test('seeded feed uses product copy instead of engineering placeholder captions', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-demo-content' })
  const response = await app.inject({ method: 'GET', url: '/v1/feed', headers: { authorization: 'Bearer demo-token' } })
  assert.equal(response.statusCode, 200)
  assert.equal(response.json().items.some((item: any) => String(item.caption).startsWith('Demo photo')), false)
  assert.deepEqual(response.json().items.slice(0, 3).map((item: any) => item.circle), ['傍晚的天空', '小圈', '宿舍窗台'])
  await app.close()
})
