import test from 'node:test'
import assert from 'node:assert/strict'
import { PrismaPhotoRepository } from '../src/prisma-photo-repository.js'

const photo = {
  id: '11111111-1111-4111-8111-111111111111',
  authorId: '22222222-2222-4222-8222-222222222222',
  filterId: 'film',
  playType: 'ccd',
  beauty: 24,
  sticker: 'star',
  caption: '窗台的风',
  circle: '小圈',
  width: 320,
  height: 240,
  createdAt: '2026-08-28T03:00:00.000Z',
  original: Buffer.from([1, 2]),
  processed: Buffer.from([3, 4]),
  idempotencyKey: 'web-1',
  deviceId: 'web',
}

function fakePrisma(record: any = null) {
  const calls: Array<{ method: string; args: any }> = []
  return {
    calls,
    photo: {
      create: async (args: any) => { calls.push({ method: 'create', args }); return args.data },
      findUnique: async (args: any) => { calls.push({ method: 'findUnique', args }); return record },
      delete: async (args: any) => { calls.push({ method: 'delete', args }); return record },
    },
    idempotencyKey: {
      findUnique: async (args: any) => { calls.push({ method: 'idempotency.findUnique', args }); return record ? { photo: record } : null },
    },
  }
}

test('persists photo metadata and derives stable OSS keys', async () => {
  const client = fakePrisma()
  const repository = new PrismaPhotoRepository(client as any)
  await repository.create(photo)
  assert.deepEqual(client.calls[0], {
    method: 'create',
    args: {
      data: {
        id: photo.id,
        authorId: photo.authorId,
        ossKey: `photos/${photo.authorId}/${photo.id}.jpg`,
        originalOssKey: `photos/${photo.authorId}/${photo.id}-original.jpg`,
        filterId: photo.filterId,
        playType: photo.playType,
        beauty: photo.beauty,
        sticker: photo.sticker,
        circle: photo.circle,
        caption: photo.caption,
        width: photo.width,
        height: photo.height,
        sizeBytes: photo.processed.length,
        createdAt: new Date(photo.createdAt),
      },
    },
  })
})

test('maps persisted records and looks up an upload by device idempotency key', async () => {
  const record = { ...photo, createdAt: new Date(photo.createdAt), ossKey: `photos/${photo.authorId}/${photo.id}.jpg`, originalOssKey: `photos/${photo.authorId}/${photo.id}-original.jpg`, sizeBytes: 2 }
  const client = fakePrisma(record)
  const repository = new PrismaPhotoRepository(client as any)
  const found = await repository.findById(photo.id)
  assert.equal(found?.id, photo.id)
  assert.equal(found?.createdAt, photo.createdAt)
  const existing = await repository.findByIdempotency(photo.authorId, photo.deviceId!, photo.idempotencyKey!)
  assert.equal(existing?.id, photo.id)
  assert.deepEqual(client.calls.at(-1), { method: 'idempotency.findUnique', args: { where: { deviceId_key: { deviceId: 'web', key: 'web-1' } }, include: { photo: true } } })
})

test('deletes metadata by photo id', async () => {
  const client = fakePrisma()
  await new PrismaPhotoRepository(client as any).remove(photo.id)
  assert.deepEqual(client.calls[0], { method: 'delete', args: { where: { id: photo.id } } })
})

