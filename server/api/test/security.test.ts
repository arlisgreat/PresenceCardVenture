import assert from 'node:assert/strict'
import test from 'node:test'
import { buildApp } from '../src/app.js'

test('adds traceable request and baseline security headers to responses', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-security' })
  const response = await app.inject({ method: 'GET', url: '/health', headers: { 'x-request-id': 'demo-trace-42' } })
  assert.equal(response.statusCode, 200)
  assert.equal(response.headers['x-request-id'], 'demo-trace-42')
  assert.equal(response.headers['x-content-type-options'], 'nosniff')
  assert.equal(response.headers['x-frame-options'], 'DENY')
  assert.equal(response.headers['referrer-policy'], 'strict-origin-when-cross-origin')
  await app.close()
})

test('replaces malformed request ids instead of reflecting untrusted header values', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-security-invalid' })
  const response = await app.inject({ method: 'GET', url: '/health', headers: { 'x-request-id': 'bad id with spaces'.repeat(10) } })
  assert.equal(response.statusCode, 200)
  assert.match(String(response.headers['x-request-id']), /^[0-9a-f-]{36}$/)
  await app.close()
})
