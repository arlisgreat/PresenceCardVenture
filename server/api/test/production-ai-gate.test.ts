import assert from 'node:assert/strict'
import test from 'node:test'
import { buildApp } from '../src/app.js'

test('production demo identities cannot spend on a configured model even if liveness is healthy', async () => {
  let calls = 0
  const app = await buildApp({
    requireProductionServices: true,
    aiProvider: { name: 'qwen', async generate() { calls++; return { status: 'failed' } } },
  })
  const response = await app.inject({
    method: 'POST', url: '/v1/ai/jobs', headers: { authorization: 'Bearer demo-token' },
    payload: { material_ids: ['p_demo_2', 'p_demo_4'] },
  })
  assert.equal(response.statusCode, 503)
  assert.equal(calls, 0)
  await app.close()
})

test('readiness inspects the actual provider rather than trusting the environment label', async () => {
  const saved = process.env.AI_PROVIDER
  process.env.AI_PROVIDER = 'qwen'
  const app = await buildApp({ requireProductionServices: true })
  try {
    const response = await app.inject({ method: 'GET', url: '/health/ready' })
    assert.equal(response.json().checks.ai_provider, false)
  } finally {
    await app.close()
    if (saved === undefined) delete process.env.AI_PROVIDER
    else process.env.AI_PROVIDER = saved
  }
})
