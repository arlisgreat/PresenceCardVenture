import test from 'node:test'
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { PrismaDeviceStore } from '../src/prisma-device-store.js'

function fakePrisma(device: any) {
  const calls: Array<{ method: string; args: any }> = []
  let current = device
  return {
    calls,
    device: {
      upsert: async (args: any) => { calls.push({ method: 'upsert', args }); current = { ...current, ...args.create, ...args.update }; return current },
      findUnique: async (args: any) => { calls.push({ method: 'findUnique', args }); return current },
      findFirst: async (args: any) => { calls.push({ method: 'findFirst', args }); return current },
      update: async (args: any) => { calls.push({ method: 'update', args }); current = { ...current, ...args.data }; return current },
    },
  }
}

test('persists a short-lived pairing code and firmware metadata', async () => {
  const client = fakePrisma({ id: 'dvc-1' })
  const store = new PrismaDeviceStore(client as any)
  const expiresAt = new Date('2026-08-28T12:10:00.000Z')

  await store.savePairCode('dvc-1', '482913', expiresAt, '0.2.0')
  assert.deepEqual(client.calls[0], {
    method: 'upsert',
    args: {
      where: { id: 'dvc-1' },
      create: { id: 'dvc-1', pairCode: '482913', pairExpiresAt: expiresAt, fwVersion: '0.2.0' },
      update: { pairCode: '482913', pairExpiresAt: expiresAt, fwVersion: '0.2.0' },
    },
  })
})

test('binds an unexpired code and stores only a hash of the device token', async () => {
  const expiresAt = new Date('2026-08-28T12:10:00.000Z')
  const client = fakePrisma({ id: 'dvc-1', pairCode: '482913', pairExpiresAt: expiresAt, userId: null })
  const store = new PrismaDeviceStore(client as any, () => new Date('2026-08-28T12:00:00.000Z'), () => 'device-secret')

  const result = await store.bind('dvc-1', '482913', 'u_demo_1')
  assert.deepEqual(result, { deviceId: 'dvc-1', deviceToken: 'device-secret' })
  const update = client.calls.find(call => call.method === 'update')
  assert.equal(update?.args.data.userId, 'u_demo_1')
  assert.equal(update?.args.data.tokenHash, createHash('sha256').update('device-secret').digest('hex'))
})

test('rejects missing, mismatched, and expired pairing codes', async () => {
  const now = new Date('2026-08-28T12:00:00.000Z')
  const missing = new PrismaDeviceStore(fakePrisma(null) as any, () => now, () => 'token')
  await assert.rejects(() => missing.bind('missing', '482913', 'u_demo_1'), /PAIR_EXPIRED/)

  const wrong = new PrismaDeviceStore(fakePrisma({ pairCode: '482913', pairExpiresAt: new Date('2026-08-28T12:10:00.000Z') }) as any, () => now, () => 'token')
  await assert.rejects(() => wrong.bind('dvc-1', '000000', 'u_demo_1'), /PAIR_EXPIRED/)

  const expired = new PrismaDeviceStore(fakePrisma({ pairCode: '482913', pairExpiresAt: new Date('2026-08-28T11:59:00.000Z') }) as any, () => now, () => 'token')
  await assert.rejects(() => expired.bind('dvc-1', '482913', 'u_demo_1'), /PAIR_EXPIRED/)
})

test('encrypts the token for status polling while retaining a hash for lookup', async () => {
  const expiresAt = new Date('2026-08-28T12:10:00.000Z')
  const client = fakePrisma({ id: 'dvc-1', pairCode: '482913', pairExpiresAt: expiresAt, userId: null })
  const store = new PrismaDeviceStore(client as any, () => new Date('2026-08-28T12:00:00.000Z'), () => 'device-secret', { encryptionKey: 'pairing-key' })

  await store.bind('dvc-1', '482913', 'u_demo_1')
  const update = client.calls.find(call => call.method === 'update')
  assert.equal(typeof update?.args.data.tokenCiphertext, 'string')
  assert.notEqual(update?.args.data.tokenCiphertext, 'device-secret')
  const status = await store.status('dvc-1', '482913')
  assert.deepEqual(status, { status: 'bound', deviceToken: 'device-secret', userId: 'u_demo_1' })
})

test('requires an encryption key when configured for production pairing', async () => {
  const client = fakePrisma({ id: 'dvc-1', pairCode: '482913', pairExpiresAt: new Date('2026-08-28T12:10:00.000Z'), userId: null })
  const store = new PrismaDeviceStore(client as any, () => new Date('2026-08-28T12:00:00.000Z'), () => 'device-secret', { requireEncryption: true })
  await assert.rejects(() => store.bind('dvc-1', '482913', 'u_demo_1'), /DEVICE_TOKEN_KEY_MISSING/)
})

test('resolves a device bearer token by hash for subsequent device requests', async () => {
  const client = fakePrisma({ id: 'dvc-1', userId: 'u_demo_1', tokenHash: createHash('sha256').update('device-secret').digest('hex') })
  const store = new PrismaDeviceStore(client as any)
  assert.deepEqual(await store.deviceForToken('device-secret'), { id: 'dvc-1', userId: 'u_demo_1' })
  const lookup = client.calls.find(call => call.method === 'findFirst')
  assert.deepEqual(lookup?.args, { where: { tokenHash: createHash('sha256').update('device-secret').digest('hex') }, include: { user: true } })
})
