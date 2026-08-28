import test from 'node:test'
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { PrismaSessionStore } from '../src/prisma-session-store.js'
import { buildApp } from '../src/app.js'

const user = { id: 'u_demo_1', username: 'ayan', displayName: '阿岩', friendCode: '100001' }

function fakePrisma(session: any) {
  const calls: any[] = []
  return {
    calls,
    session: {
      findUnique: async (args: any) => {
        calls.push(args)
        return session
      },
    },
  }
}

test('resolves a non-expired session by hashing the bearer token', async () => {
  const token = 'session-secret'
  const client = fakePrisma({ id: 's1', tokenHash: createHash('sha256').update(token).digest('hex'), expiresAt: new Date(Date.now() + 60_000), revokedAt: null, user })
  const store = new PrismaSessionStore(client as any)

  assert.deepEqual(await store.userForToken(token), user)
  assert.deepEqual(client.calls[0], { where: { tokenHash: createHash('sha256').update(token).digest('hex') }, include: { user: true } })
})

test('rejects expired and revoked sessions without exposing the user', async () => {
  const token = 'session-secret'
  const expired = fakePrisma({ tokenHash: 'hash', expiresAt: new Date(Date.now() - 1), revokedAt: null, user })
  const revoked = fakePrisma({ tokenHash: 'hash', expiresAt: new Date(Date.now() + 60_000), revokedAt: new Date(), user })

  assert.equal(await new PrismaSessionStore(expired as any).userForToken(token), undefined)
  assert.equal(await new PrismaSessionStore(revoked as any).userForToken(token), undefined)
})

test('uses the injected session store at the profile request boundary', async () => {
  const app = await buildApp({
    uploadsDir: '/tmp/presence-card-test-prisma-session-boundary',
    authStore: {
      provider: 'prisma',
      userForToken: async (token?: string) => token === 'real-token' ? user : undefined,
    },
  })
  const accepted = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: 'Bearer real-token' } })
  assert.equal(accepted.statusCode, 200)
  assert.equal(accepted.json().username, 'ayan')
  const rejected = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: 'Bearer demo-token' } })
  assert.equal(rejected.statusCode, 401)
  await app.close()
})
