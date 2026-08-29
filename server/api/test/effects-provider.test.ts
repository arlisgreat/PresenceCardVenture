import test from 'node:test'
import assert from 'node:assert/strict'
import { deflateSync } from 'node:zlib'
import { buildApp } from '../src/app.js'
import { DemoStore, type Photo } from '../src/demo-store.js'
import { PresenceEffectsAiProvider, type MaterialAuthorizer, type MaterialConsentContext } from '../src/effects-provider.js'
import type { ImageProvider } from '@pvc/effects/providers'
import type { AiGenerationInput } from '../src/ai-provider.js'

// Original synthetic images: no external assets, real people, network, or API keys.
function png(width: number, height: number, rgb: [number, number, number]): Buffer {
  const chunk = (type: string, data: Buffer) => {
    const name = Buffer.from(type)
    const input = Buffer.concat([name, data])
    let crc = 0xffffffff
    for (const byte of input) { crc ^= byte; for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0) }
    const length = Buffer.alloc(4); length.writeUInt32BE(data.length)
    const checksum = Buffer.alloc(4); checksum.writeUInt32BE((crc ^ 0xffffffff) >>> 0)
    return Buffer.concat([length, input, checksum])
  }
  const header = Buffer.alloc(13)
  header.writeUInt32BE(width); header.writeUInt32BE(height, 4); header[8] = 8; header[9] = 2
  const pixels = Buffer.alloc(height * (1 + width * 3))
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    const offset = y * (1 + width * 3) + 1 + x * 3
    pixels[offset] = rgb[0]; pixels[offset + 1] = rgb[1]; pixels[offset + 2] = rgb[2]
  }
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', header), chunk('IDAT', deflateSync(pixels)), chunk('IEND', Buffer.alloc(0))])
}

const auth = { authorization: 'Bearer demo-token' }
const jsonAuth = { ...auth, 'content-type': 'application/json' }
const otherAuth = { authorization: 'Bearer demo-user-2' }
const output = png(512, 384, [236, 211, 185])

function fixtures(options: { authorizeMaterial?: MaterialAuthorizer; generate?: ImageProvider['generate']; maxConcurrent?: number } = {}) {
  const store = new DemoStore()
  store.photos.clear()
  const saved = new Map<string, Photo>()
  const originalsRead: string[] = []
  const originals = new Map<string, Buffer>()
  for (const [id, ownerId, bytes] of [
    ['source-a', 'u_demo_1', png(320, 240, [220, 180, 140])],
    ['source-b', 'u_demo_2', png(320, 240, [130, 180, 220])],
  ] as const) {
    const photo: Photo = {
      id, authorId: ownerId, filterId: 'none', caption: null, width: 320, height: 240,
      createdAt: new Date().toISOString(), original: Buffer.alloc(0), processed: Buffer.from('not-the-original'),
    }
    store.photos.set(id, photo)
    originals.set(id, bytes)
  }
  const files = {
    async save(photo: Photo) { saved.set(photo.id, { ...photo, original: Buffer.from(photo.original), processed: Buffer.from(photo.processed) }) },
    async read(photo: Photo) { return saved.get(photo.id)?.processed ?? photo.processed },
    async readOriginal(photo: Photo) { originalsRead.push(photo.id); const bytes = originals.get(photo.id) ?? saved.get(photo.id)?.original; if (!bytes) throw new Error('missing original'); return bytes },
    async remove(photo: Photo) { saved.delete(photo.id) },
  }
  const calls: Array<Parameters<ImageProvider['generate']>[0]> = []
  const imageProvider: ImageProvider = {
    name: 'fake-image-provider', model: 'test-model',
    async generate(input) { calls.push(input); return options.generate ? options.generate(input) : { bytes: output, mimeType: 'image/png', model: 'test-model', requestId: 'test-request' } },
  }
  const provider = new PresenceEffectsAiProvider({ store, files, imageProvider, authorizeMaterial: options.authorizeMaterial, maxConcurrent: options.maxConcurrent })
  const input: AiGenerationInput = { materialIds: ['source-a', 'source-b'], actorId: 'u_demo_1', jobId: 'job_test' }
  return { store, files, saved, originals, originalsRead, calls, provider, imageProvider, input }
}

async function awaitJob(app: Awaited<ReturnType<typeof buildApp>>, id: string) {
  for (let attempt = 0; attempt < 500; attempt++) {
    const result = await app.inject({ method: 'GET', url: `/v1/ai/jobs/${id}`, headers: auth })
    const job = result.json()
    if (job.status === 'completed' || job.status === 'failed') return job
    await new Promise(resolve => setTimeout(resolve, 10))
  }
  throw new Error('local synthetic job did not settle')
}

test('real adapter reads originals, invokes the model once, and stores new JPEG draft bytes', async () => {
  const grants: MaterialConsentContext[] = []
  const f = fixtures({ authorizeMaterial: async context => { grants.push(context); return true } })
  const [first, duplicate] = await Promise.all([f.provider.generate(f.input), f.provider.generate(f.input)])
  assert.equal(first.status, 'completed')
  assert.deepEqual(duplicate, first)
  assert.equal(f.calls.length, 1)
  assert.deepEqual(f.originalsRead, ['source-a', 'source-b'])
  assert.equal(f.calls[0].images.length, 2)
  assert.ok(f.calls[0].images.every(image => image.mimeType === 'image/jpeg' && image.bytes[0] === 0xff && image.bytes[1] === 0xd8))
  assert.ok(grants.length >= 2)
  assert.ok(grants.every(context => context.materialId === 'source-b' && context.ownerId === 'u_demo_2' && context.provider === 'fake-image-provider' && context.model === 'test-model' && context.purpose === 'generate'))
  assert.ok(!f.input.materialIds.includes(first.resultPhotoId!))
  const photo = f.saved.get(first.resultPhotoId!)!
  assert.equal(photo.authorId, f.input.actorId)
  assert.equal(photo.draftJobId, f.input.jobId)
  assert.equal(photo.aiGenerated, true)
  assert.equal(photo.filterId, 'warm')
  assert.deepEqual(photo.original, output)
  assert.equal(photo.processed[0], 0xff)
  assert.equal(photo.processed[1], 0xd8)
  assert.equal(photo.width, 512)
  assert.equal(photo.height, 384)
  assert.equal(photo.generation?.requestId, 'test-request')
  assert.equal(photo.generation?.intensity, 0.6)
  assert.equal(typeof photo.generation?.seed, 'number')
  assert.ok(photo.generation?.referenceWarnings?.includes('LOW_RESOLUTION_REFERENCE'))
})

test('caller consent:true and friendship never authorize a different owner', async () => {
  const f = fixtures()
  const app = await buildApp({ store: f.store, aiProvider: f.provider, photoStorage: f.files })
  try {
    const response = await app.inject({ method: 'POST', url: '/v1/ai/jobs', headers: jsonAuth, payload: { material_ids: f.input.materialIds, consent: true } })
    assert.equal(response.statusCode, 403)
    assert.equal(response.json().error.code, 'AI_MATERIAL_FORBIDDEN')
    assert.equal(f.store.jobs.size, 0)
    assert.equal(f.calls.length, 0)
  } finally { await app.close() }
})

test('revoked or unavailable consent is checked again immediately before a model call', async () => {
  let allowed = true
  const f = fixtures({ authorizeMaterial: async () => allowed })
  await f.provider.authorize(f.input)
  allowed = false
  assert.equal((await f.provider.generate(f.input)).errorCode, 'AI_MATERIAL_FORBIDDEN')
  assert.equal(f.calls.length, 0)
  const unavailable = fixtures({ authorizeMaterial: async () => { throw new Error('database password must stay private') } })
  const result = await unavailable.provider.generate(unavailable.input)
  assert.equal(result.errorCode, 'AI_CONSENT_UNAVAILABLE')
  assert.ok(!result.error?.includes('password'))
  assert.equal(unavailable.calls.length, 0)
})

test('duplicate material IDs, duplicate images, and tiny actual images are rejected before billing', async () => {
  const duplicate = fixtures({ authorizeMaterial: async () => true })
  assert.equal((await duplicate.provider.generate({ ...duplicate.input, materialIds: ['source-a', 'source-a'] })).errorCode, 'AI_MATERIAL_INVALID')
  assert.equal(duplicate.calls.length, 0)
  const sameBytes = fixtures({ authorizeMaterial: async () => true })
  sameBytes.originals.set('source-b', sameBytes.originals.get('source-a')!)
  assert.equal((await sameBytes.provider.generate(sameBytes.input)).errorCode, 'AI_MATERIAL_INVALID')
  assert.equal(sameBytes.calls.length, 0)
  const tiny = fixtures({ authorizeMaterial: async () => true })
  tiny.originals.set('source-a', png(16, 16, [120, 150, 190]))
  assert.equal((await tiny.provider.generate(tiny.input)).errorCode, 'AI_REFERENCE_TOO_SMALL')
  assert.equal(tiny.calls.length, 0)
})

test('draft stays private; concurrent publication is idempotent and copies the actual result', async () => {
  const grants: MaterialConsentContext[] = []
  const f = fixtures({ authorizeMaterial: async context => { grants.push(context); return true } })
  const app = await buildApp({ store: f.store, aiProvider: f.provider, photoStorage: f.files })
  try {
    const created = await app.inject({ method: 'POST', url: '/v1/ai/jobs', headers: jsonAuth, payload: { photo_ids: f.input.materialIds } })
    assert.equal(created.statusCode, 202)
    const job = await awaitJob(app, created.json().job_id)
    assert.equal(job.status, 'completed', job.message)
    const draftId = job.resultPhotoId
    for (const headers of [auth, otherAuth]) {
      const feed = await app.inject({ method: 'GET', url: '/v1/feed', headers })
      assert.equal(feed.json().items.some((photo: any) => photo.photo_id === draftId), false)
    }
    const mine = await app.inject({ method: 'GET', url: '/v1/photos/mine', headers: auth })
    assert.equal(mine.json().items.some((photo: any) => photo.photo_id === draftId), false)
    const privateImage = await app.inject({ method: 'GET', url: job.result_url, headers: otherAuth })
    assert.equal(privateImage.statusCode, 403)
    const ownImage = await app.inject({ method: 'GET', url: job.result_url, headers: auth })
    assert.equal(ownImage.statusCode, 200)
    assert.deepEqual(ownImage.rawPayload, f.saved.get(draftId)!.processed)
    const [a, b] = await Promise.all([1, 2].map(() => app.inject({ method: 'POST', url: `/v1/ai/jobs/${job.id}/publish`, headers: jsonAuth, payload: { caption: '一起在场', circle: '小圈' } })))
    assert.deepEqual([a.statusCode, b.statusCode].sort(), [200, 201])
    assert.equal(a.json().photo_id, b.json().photo_id)
    const published = f.store.photos.get(a.json().photo_id)!
    assert.equal(published.draftJobId, undefined)
    assert.equal(published.aiGenerated, true)
    assert.equal(published.caption, 'AI 合照 · 一起在场')
    assert.equal(a.json().caption, 'AI 合照 · 一起在场')
    assert.deepEqual(published.processed, ownImage.rawPayload)
    assert.deepEqual(published.original, output)
    assert.ok(grants.some(context => context.purpose === 'publish'))
    assert.equal(f.calls.length, 1)
    const removed = await app.inject({ method: 'DELETE', url: `/v1/ai/jobs/${job.id}/result`, headers: auth })
    assert.equal(removed.statusCode, 204)
    assert.equal(f.store.photos.has(draftId), false)
    assert.equal(f.saved.has(draftId), false)
    assert.ok(f.store.photos.has(published.id))
    assert.ok(f.input.materialIds.every(id => f.store.photos.has(id)))
  } finally { await app.close() }
})

test('publication needs a separate current consent grant', async () => {
  const f = fixtures({ authorizeMaterial: async context => context.purpose === 'generate' })
  const app = await buildApp({ store: f.store, aiProvider: f.provider, photoStorage: f.files })
  try {
    const response = await app.inject({ method: 'POST', url: '/v1/ai/jobs', headers: jsonAuth, payload: { material_ids: f.input.materialIds } })
    const job = await awaitJob(app, response.json().job_id)
    assert.equal(job.status, 'completed')
    const publish = await app.inject({ method: 'POST', url: `/v1/ai/jobs/${job.id}/publish`, headers: jsonAuth, payload: {} })
    assert.equal(publish.statusCode, 403)
    assert.equal(f.store.visiblePhotos(f.input.actorId).length, 2)
  } finally { await app.close() }
})

test('provider failures are redacted and never become an original-photo success', async () => {
  const f = fixtures({ authorizeMaterial: async () => true, generate: async () => { throw Object.assign(new Error('Authorization: Bearer secret-response-body'), { code: 'PROVIDER_AUTH' }) } })
  const app = await buildApp({ store: f.store, aiProvider: f.provider, photoStorage: f.files })
  try {
    const response = await app.inject({ method: 'POST', url: '/v1/ai/jobs', headers: jsonAuth, payload: { material_ids: f.input.materialIds } })
    const job = await awaitJob(app, response.json().job_id)
    assert.equal(job.status, 'failed')
    assert.equal(job.result_url, undefined)
    assert.equal(job.resultPhotoId, undefined)
    assert.equal(job.message, 'Image generation failed. Please try again later.')
    assert.ok(!JSON.stringify(job).includes('secret-response-body'))
    assert.equal(f.calls.length, 1)
    assert.equal(f.saved.size, 0)
  } finally { await app.close() }
})

test('concurrency is bounded and a full adapter never automatically retries a billed call', async () => {
  let release!: () => void
  const gate = new Promise<void>(resolve => { release = resolve })
  const f = fixtures({ maxConcurrent: 1, authorizeMaterial: async () => true, generate: async () => { await gate; return { bytes: output, mimeType: 'image/png', model: 'test-model' } } })
  const first = f.provider.generate(f.input)
  const busy = await f.provider.generate({ ...f.input, jobId: 'job_other' })
  assert.equal(busy.status, 'failed')
  assert.equal(busy.errorCode, 'AI_BUSY')
  release()
  assert.equal((await first).status, 'completed')
  assert.equal(f.calls.length, 1)
})

test('an adapter cannot mark an input photo as a successful real AI result', async () => {
  const f = fixtures()
  f.store.photos.get('source-b')!.authorId = f.input.actorId
  const app = await buildApp({
    store: f.store, photoStorage: f.files,
    aiProvider: { name: 'not-a-simulator', generate: async input => ({ status: 'completed', resultPhotoId: input.materialIds[0] }) },
  })
  try {
    const response = await app.inject({ method: 'POST', url: '/v1/ai/jobs', headers: jsonAuth, payload: { material_ids: f.input.materialIds } })
    const job = await awaitJob(app, response.json().job_id)
    assert.equal(job.status, 'failed')
    assert.equal(job.result_url, undefined)
    assert.equal(job.message, 'The generated image is unavailable.')
  } finally { await app.close() }
})

test('missing originals and invalid model output fail without publishing any image', async () => {
  const missing = fixtures({ authorizeMaterial: async () => true })
  missing.originals.clear()
  assert.equal((await missing.provider.generate(missing.input)).errorCode, 'AI_REFERENCE_UNAVAILABLE')
  assert.equal(missing.calls.length, 0)
  const invalid = fixtures({ authorizeMaterial: async () => true, generate: async () => ({ bytes: Buffer.from('not-an-image'), mimeType: 'image/png', model: 'test-model' }) })
  assert.equal((await invalid.provider.generate(invalid.input)).errorCode, 'AI_RESULT_UNAVAILABLE')
  assert.equal(invalid.saved.size, 0)
})

test('deleting a running job prevents a late model response from saving a draft', async () => {
  let release!: () => void
  let started!: () => void
  const gate = new Promise<void>(resolve => { release = resolve })
  const running = new Promise<void>(resolve => { started = resolve })
  const f = fixtures({ authorizeMaterial: async () => true, generate: async () => {
    started()
    await gate
    return { bytes: output, mimeType: 'image/png', model: 'test-model' }
  } })
  const app = await buildApp({ store: f.store, photoStorage: f.files, aiProvider: f.provider })
  try {
    const created = await app.inject({ method: 'POST', url: '/v1/ai/jobs', headers: jsonAuth, payload: { material_ids: f.input.materialIds } })
    await running
    const removed = await app.inject({ method: 'DELETE', url: `/v1/ai/jobs/${created.json().job_id}/result`, headers: auth })
    assert.equal(removed.statusCode, 204)
    release()
    await new Promise(resolve => setTimeout(resolve, 20))
    assert.equal(f.calls.length, 1)
    assert.equal(f.saved.size, 0)
    assert.equal(f.store.photos.size, 2)
    assert.equal(f.store.jobs.size, 0)
  } finally { release(); await app.close() }
})

test('completed and failed jobs deduplicate through store terminal state without a lifetime cache limit', async () => {
  const f = fixtures({ authorizeMaterial: async () => true })
  const first = await f.provider.generate(f.input)
  assert.equal(first.status, 'completed')
  f.store.jobs.set(f.input.jobId, { id: f.input.jobId, ownerId: f.input.actorId, materialIds: f.input.materialIds, status: 'completed', resultPhotoId: first.resultPhotoId, createdAt: new Date().toISOString() })
  assert.deepEqual(await f.provider.generate(f.input), first)
  for (let index = 0; index < 300; index++) {
    const id = `old-${index}`
    f.store.jobs.set(id, { id, ownerId: f.input.actorId, materialIds: f.input.materialIds, status: 'failed', createdAt: new Date().toISOString() })
  }
  assert.equal((await f.provider.generate({ ...f.input, jobId: 'old-1' })).status, 'failed')
  assert.equal(f.calls.length, 1)
  assert.equal((await f.provider.generate({ ...f.input, jobId: 'fresh' })).status, 'completed')
  assert.equal(f.calls.length, 2)
})

test('AI label cannot be removed and counts toward the existing 140 character caption limit', async () => {
  const f = fixtures({ authorizeMaterial: async () => true })
  const app = await buildApp({ store: f.store, photoStorage: f.files, aiProvider: f.provider })
  try {
    const created = await app.inject({ method: 'POST', url: '/v1/ai/jobs', headers: jsonAuth, payload: { material_ids: f.input.materialIds } })
    const job = await awaitJob(app, created.json().job_id)
    const tooLong = await app.inject({ method: 'POST', url: `/v1/ai/jobs/${job.id}/publish`, headers: jsonAuth, payload: { caption: 'x'.repeat(140) } })
    assert.equal(tooLong.statusCode, 400)
    const once = await app.inject({ method: 'POST', url: `/v1/ai/jobs/${job.id}/publish`, headers: jsonAuth, payload: { caption: 'AI 合照 · 我们' } })
    assert.equal(once.statusCode, 201)
    assert.equal(once.json().caption, 'AI 合照 · 我们')
  } finally { await app.close() }
})

test('private drafts cannot leak through footprints, messages, or reactions', async () => {
  const f = fixtures({ authorizeMaterial: async () => true })
  const app = await buildApp({ store: f.store, photoStorage: f.files, aiProvider: f.provider })
  try {
    const created = await app.inject({ method: 'POST', url: '/v1/ai/jobs', headers: jsonAuth, payload: { material_ids: f.input.materialIds } })
    const job = await awaitJob(app, created.json().job_id)
    assert.equal(job.status, 'completed')
    const draftId = job.resultPhotoId
    for (const headers of [auth, otherAuth]) {
      const footprints = await app.inject({ method: 'GET', url: '/v1/footprints', headers })
      assert.equal(footprints.statusCode, 200)
      assert.equal(footprints.json().some((photo: any) => photo.photo_id === draftId), false)
      const message = await app.inject({ method: 'POST', url: '/v1/messages', headers: { ...headers, 'content-type': 'application/json' }, payload: { to: headers === auth ? 'momo' : 'ayan', photo_id: draftId } })
      assert.equal(message.statusCode, 403)
      const reaction = await app.inject({ method: 'POST', url: `/v1/photos/${draftId}/reactions`, headers: { ...headers, 'content-type': 'application/json' }, payload: { type: 'heart' } })
      assert.equal(reaction.statusCode, 403)
      const undo = await app.inject({ method: 'DELETE', url: `/v1/photos/${draftId}/reactions/heart`, headers })
      assert.equal(undo.statusCode, 403)
    }
    assert.equal(f.store.messages.length, 0)
    assert.equal(f.store.reactions.size, 0)
  } finally { await app.close() }
})

test('buildApp wires an image provider and material authorizer into the real AI adapter', async () => {
  const f = fixtures()
  const grants: MaterialConsentContext[] = []
  const app = await buildApp({
    store: f.store, photoStorage: f.files, imageProvider: f.imageProvider,
    authorizeAiMaterial: async context => { grants.push(context); return true },
  })
  try {
    const created = await app.inject({ method: 'POST', url: '/v1/ai/jobs', headers: jsonAuth, payload: { material_ids: f.input.materialIds } })
    assert.equal(created.statusCode, 202)
    assert.equal(created.json().provider, f.imageProvider.name)
    const job = await awaitJob(app, created.json().job_id)
    assert.equal(job.status, 'completed')
    assert.equal(f.calls.length, 1)
    assert.ok(grants.length >= 2)
    assert.equal(f.store.photos.get(job.resultPhotoId)?.aiGenerated, true)
    const ready = await app.inject({ method: 'GET', url: '/health/ready' })
    assert.equal(ready.json().checks.ai_provider, true)
  } finally { await app.close() }
})
