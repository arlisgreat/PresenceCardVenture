import type { FastifyInstance, FastifyRequest } from 'fastify'
import { randomUUID } from 'node:crypto'
import { DemoStore, errorBody, type Job, type Photo } from '../demo-store.js'
import { AiProviderError, SimulatorAiProvider, safeAiError, type AiGenerationInput, type AiProvider } from '../ai-provider.js'
import type { PhotoStorage } from '../photo-store.js'

const auth = (request: FastifyRequest, store: DemoStore) => store.userForToken(String(request.headers.authorization ?? '').replace(/^Bearer\s+/i, ''))
const inputFor = (job: Job, signal?: AbortSignal): AiGenerationInput => ({ materialIds: [...job.materialIds], actorId: job.ownerId, jobId: job.id, signal })
const isSimulation = (provider: AiProvider) => provider.name === 'simulator'

export async function aiRoutes(app: FastifyInstance, store: DemoStore, provider: AiProvider = new SimulatorAiProvider(), files?: PhotoStorage) {
  const pending = new Map<string, { controller: AbortController; timer: ReturnType<typeof setTimeout>; started: boolean }>()
  const mutations = new Map<string, Promise<unknown>>()

  async function locked<T>(id: string, operation: () => Promise<T>): Promise<T> {
    const previous = mutations.get(id) ?? Promise.resolve()
    const next = previous.catch(() => undefined).then(operation)
    mutations.set(id, next)
    try { return await next } finally { if (mutations.get(id) === next) mutations.delete(id) }
  }

  async function authorize(input: AiGenerationInput, purpose: 'generate' | 'publish') {
    for (const id of input.materialIds) {
      const photo = store.photos.get(id)
      if (!photo || photo.draftJobId || (photo.authorId !== input.actorId && !provider.authorize)) throw new AiProviderError('AI_MATERIAL_FORBIDDEN')
    }
    // Friendship and the caller's consent:true flag never grant another owner's rights.
    await provider.authorize?.(input, purpose)
  }

  app.addHook('onClose', async () => {
    for (const task of pending.values()) { task.controller.abort(); if (!task.started) clearTimeout(task.timer) }
    pending.clear()
  })

  app.post('/ai/jobs', async (request, reply) => {
    const user = auth(request, store)
    if (!user) return reply.code(401).send(errorBody('TOKEN_INVALID', 'token invalid'))
    const body = request.body as { material_ids?: unknown; photo_ids?: unknown } | undefined
    const ids = body?.material_ids ?? body?.photo_ids
    if (!Array.isArray(ids) || ids.length !== 2 || new Set(ids).size !== 2 || ids.some(id => typeof id !== 'string' || !id || id.length > 256)) {
      return reply.code(400).send(errorBody('AI_MATERIAL_INVALID', 'Choose two different image materials.'))
    }
    const job: Job = { id: `job_${randomUUID()}`, ownerId: user.id, materialIds: [...ids], provider: provider.name, status: 'queued', createdAt: new Date().toISOString() }
    try { await authorize(inputFor(job), 'generate') } catch (error) {
      const safe = safeAiError(error)
      return reply.code(safe.statusCode).send(errorBody(safe.code, safe.message))
    }
    if (pending.size >= 16) return reply.code(429).header('Retry-After', '2').send(errorBody('AI_BUSY', 'Image generation is busy. Please try again later.'))
    store.jobs.set(job.id, job)
    const controller = new AbortController()
    const task = { controller, started: false, timer: undefined as unknown as ReturnType<typeof setTimeout> }
    task.timer = setTimeout(async () => {
      task.started = true
      try {
        if (controller.signal.aborted || !store.jobs.has(job.id)) return
        job.status = 'processing'
        const input = inputFor(job, controller.signal)
        await authorize(input, 'generate')
        const result = await provider.generate(input)
        if (controller.signal.aborted || !store.jobs.has(job.id)) {
          await provider.discard?.(inputFor(job)).catch(() => undefined)
          return
        }
        if (result.status !== 'completed') throw result.errorCode ? new AiProviderError(result.errorCode) : new AiProviderError('AI_GENERATION_FAILED')
        const resultPhoto = result.resultPhotoId ? store.photos.get(result.resultPhotoId) : undefined
        if (!resultPhoto || (!isSimulation(provider) && (resultPhoto.authorId !== user.id || resultPhoto.draftJobId !== job.id || ids.includes(resultPhoto.id)))) {
          throw new AiProviderError('AI_RESULT_UNAVAILABLE')
        }
        job.status = 'completed'
        job.resultPhotoId = resultPhoto.id
        job.error = undefined
      } catch (error) {
        if (store.jobs.has(job.id)) { job.status = 'failed'; job.error = safeAiError(error).message }
      } finally {
        pending.delete(job.id)
      }
    }, 5)
    pending.set(job.id, task)
    return reply.code(202).send({ job_id: job.id, id: job.id, status: job.status, provider: job.provider, created_at: job.createdAt })
  })

  app.get('/ai/jobs/:id', async (request, reply) => {
    const user = auth(request, store)
    if (!user) return reply.code(401).send(errorBody('TOKEN_INVALID', 'token invalid'))
    const job = store.jobs.get((request.params as { id: string }).id)
    if (!job) return reply.code(404).send(errorBody('NOT_FOUND', 'job not found'))
    if (job.ownerId !== user.id) return reply.code(403).send(errorBody('FORBIDDEN', 'not owner'))
    const result = job.resultPhotoId ? store.photos.get(job.resultPhotoId) : undefined
    return {
      ...job,
      result_url: job.status === 'completed' && result ? `/v1/photos/${result.id}/image` : undefined,
      message: job.error ?? (isSimulation(provider) ? 'Simulation only: a source photo is reused; no AI image was generated.' : undefined),
    }
  })

  app.post('/ai/jobs/:id/publish', async (request, reply) => {
    const user = auth(request, store)
    if (!user) return reply.code(401).send(errorBody('TOKEN_INVALID', 'token invalid'))
    return locked((request.params as { id: string }).id, async () => {
      const job = store.jobs.get((request.params as { id: string }).id)
      if (!job) return reply.code(404).send(errorBody('NOT_FOUND', 'job not found'))
      if (job.ownerId !== user.id) return reply.code(403).send(errorBody('FORBIDDEN', 'not owner'))
      if (job.publishedPhotoId) {
        const existing = store.photos.get(job.publishedPhotoId)
        if (existing) return reply.code(200).send(publishedBody(job, existing))
        job.publishedPhotoId = undefined
      }
      if (job.status !== 'completed') return reply.code(409).send(errorBody('AI_NOT_READY', 'AI result is not ready'))
      const source = job.resultPhotoId ? store.photos.get(job.resultPhotoId) : undefined
      if (!source) return reply.code(409).send(errorBody('AI_RESULT_UNAVAILABLE', 'AI result is unavailable'))
      const body = (request.body ?? {}) as { caption?: unknown; circle?: unknown }
      const requestedCaption = body.caption === undefined ? '两份在场，遇见一次。' : String(body.caption)
      const aiLabel = 'AI 合照 · '
      const caption = source.aiGenerated && !requestedCaption.startsWith(aiLabel) ? `${aiLabel}${requestedCaption}` : requestedCaption
      const circle = body.circle === undefined ? '小圈' : String(body.circle)
      if (caption.length > 140 || !circle || circle.length > 32) return reply.code(400).send(errorBody('BAD_REQUEST', 'caption must be at most 140 characters and circle must be between 1 and 32 characters'))
      try { await authorize(inputFor(job), 'publish') } catch (error) {
        const safe = safeAiError(error)
        return reply.code(safe.statusCode).send(errorBody(safe.code, safe.message))
      }
      // A draft is owner-only. Only this explicit publish step creates a feed photo.
      const { draftJobId: _draft, ...sourceFields } = source
      const photo: Photo = {
        ...sourceFields, id: `p_ai_${randomUUID()}`, authorId: user.id, caption, circle,
        createdAt: new Date().toISOString(), idempotencyKey: undefined, deviceId: undefined,
        ...(isSimulation(provider) ? { aiGenerated: false, generation: { provider: 'simulator', model: 'none', promptVersion: 'simulation' } } : {}),
      }
      if (!files || !photo.original.length || !photo.processed.length) return reply.code(503).send(errorBody('STORAGE_UNAVAILABLE', 'Image storage is unavailable.'))
      try { await files.save(photo) } catch {
        await files.remove(photo).catch(() => undefined)
        return reply.code(503).send(errorBody('STORAGE_UNAVAILABLE', 'Image storage is unavailable.'))
      }
      store.photos.set(photo.id, photo)
      job.publishedPhotoId = photo.id
      return reply.code(201).send(publishedBody(job, photo))
    })
  })

  app.delete('/ai/jobs/:id/result', async (request, reply) => {
    const user = auth(request, store)
    if (!user) return reply.code(401).send(errorBody('TOKEN_INVALID', 'token invalid'))
    return locked((request.params as { id: string }).id, async () => {
      const job = store.jobs.get((request.params as { id: string }).id)
      if (!job) return reply.code(404).send(errorBody('NOT_FOUND', 'job not found'))
      if (job.ownerId !== user.id) return reply.code(403).send(errorBody('FORBIDDEN', 'not owner'))
      const task = pending.get(job.id)
      if (task) { task.controller.abort(); if (!task.started) { clearTimeout(task.timer); pending.delete(job.id) } }
      try { await provider.discard?.(inputFor(job)) } catch (error) {
        const safe = safeAiError(error)
        return reply.code(safe.statusCode).send(errorBody(safe.code, safe.message))
      }
      store.jobs.delete(job.id)
      return reply.code(204).send()
    })
  })
}

function publishedBody(job: Job, photo: Photo) {
  return { photo_id: photo.id, url: `/v1/photos/${photo.id}/image`, created_at: photo.createdAt, caption: photo.caption, circle: photo.circle, source_job_id: job.id }
}
