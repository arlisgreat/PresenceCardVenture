import { createHash, randomUUID } from 'node:crypto'
import { prepareReference, renderPhoto, type PresetId } from '@pvc/effects'
import { buildTogetherPrompt, PROMPT_VERSION, type ImageProvider } from '@pvc/effects/providers'
import type { DemoStore, Photo } from './demo-store.js'
import type { PhotoStorage } from './photo-store.js'
import { AiProviderError, failedAiResult, type AiGenerationInput, type AiGenerationResult, type AiProvider } from './ai-provider.js'

export type MaterialConsentContext = {
  actorId: string
  materialId: string
  ownerId: string
  jobId: string
  provider: string
  model: string
  purpose: 'generate' | 'publish'
}

export type MaterialAuthorizer = (context: MaterialConsentContext) => Promise<boolean>

type OriginalPhotoStorage = PhotoStorage & { readOriginal?(photo: Photo): Promise<Buffer> }
type Options = {
  store: DemoStore
  files: OriginalPhotoStorage
  imageProvider: ImageProvider
  authorizeMaterial?: MaterialAuthorizer
  maxConcurrent?: number
  scene?: 'window' | 'walk' | 'cafe'
  resultPresetId?: PresetId
  resultIntensity?: number
}

const MAX_REFERENCE_BYTES = 8 * 1024 * 1024
const digest = (bytes: Uint8Array) => createHash('sha256').update(bytes).digest('hex')
const fingerprint = (input: AiGenerationInput) => JSON.stringify([input.actorId, input.materialIds])

/** Adapter for the current in-process job store. Production still needs a durable queue. */
export class PresenceEffectsAiProvider implements AiProvider {
  readonly name: string
  readonly model: string
  private active = 0
  private readonly maxConcurrent: number
  private readonly resultPresetId: PresetId
  private readonly resultIntensity: number
  private readonly runs = new Map<string, { fingerprint: string; promise: Promise<AiGenerationResult> }>()

  constructor(private readonly options: Options) {
    this.name = options.imageProvider.name
    this.model = options.imageProvider.model
    this.maxConcurrent = options.maxConcurrent ?? 2
    this.resultPresetId = options.resultPresetId ?? 'warm'
    this.resultIntensity = options.resultIntensity ?? 0.6
    if (!Number.isInteger(this.maxConcurrent) || this.maxConcurrent < 1 || this.maxConcurrent > 8) throw new Error('maxConcurrent must be an integer from 1 to 8')
    if (!['none', 'warm', 'bw', 'film', 'vivid'].includes(this.resultPresetId)) throw new Error('Unknown result preset')
    if (!Number.isFinite(this.resultIntensity) || this.resultIntensity < 0 || this.resultIntensity > 1) throw new Error('resultIntensity must be from 0 to 1')
  }

  async authorize(input: AiGenerationInput, purpose: 'generate' | 'publish' = 'generate'): Promise<void> {
    if (!input.actorId || !input.jobId || input.materialIds.length !== 2 || new Set(input.materialIds).size !== 2 || input.materialIds.some(id => typeof id !== 'string' || !id || id.length > 256)) {
      throw new AiProviderError('AI_MATERIAL_INVALID')
    }
    this.assertActive(input)
    for (const id of input.materialIds) {
      const photo = this.options.store.photos.get(id)
      if (!photo || photo.draftJobId) throw new AiProviderError('AI_MATERIAL_FORBIDDEN')
      if (photo.authorId === input.actorId) continue
      if (!this.options.authorizeMaterial) throw new AiProviderError('AI_MATERIAL_FORBIDDEN')
      let allowed = false
      try {
        allowed = await this.options.authorizeMaterial({
          actorId: input.actorId, materialId: id, ownerId: photo.authorId, jobId: input.jobId,
          provider: this.name, model: this.model, purpose,
        })
      } catch {
        throw new AiProviderError('AI_CONSENT_UNAVAILABLE')
      }
      if (allowed !== true) throw new AiProviderError('AI_MATERIAL_FORBIDDEN')
      this.assertActive(input)
    }
  }

  generate(input: AiGenerationInput): Promise<AiGenerationResult> {
    const previous = this.runs.get(input.jobId)
    if (previous) return previous.fingerprint === fingerprint(input)
      ? previous.promise
      : Promise.resolve(failedAiResult(new AiProviderError('AI_MATERIAL_INVALID')))
    const job = this.options.store.jobs.get(input.jobId)
    if (job) {
      if (job.ownerId !== input.actorId || fingerprint({ ...input, actorId: job.ownerId, materialIds: job.materialIds }) !== fingerprint(input)) {
        return Promise.resolve(failedAiResult(new AiProviderError('AI_MATERIAL_INVALID')))
      }
      if (job.status === 'failed') return Promise.resolve(failedAiResult(new AiProviderError('AI_GENERATION_FAILED')))
      if (job.status === 'completed') {
        const photo = job.resultPhotoId ? this.options.store.photos.get(job.resultPhotoId) : undefined
        return Promise.resolve(photo?.authorId === input.actorId
          ? { status: 'completed', resultPhotoId: photo.id }
          : failedAiResult(new AiProviderError('AI_RESULT_UNAVAILABLE')))
      }
    }
    // A saved draft without its job record must not trigger another billed call.
    const existing = [...this.options.store.photos.values()].find(photo => photo.draftJobId === input.jobId)
    if (existing && !job) return Promise.resolve(failedAiResult(new AiProviderError('AI_RESULT_UNAVAILABLE')))
    if (existing) return Promise.resolve(existing.authorId === input.actorId
      ? { status: 'completed', resultPhotoId: existing.id }
      : failedAiResult(new AiProviderError('AI_MATERIAL_FORBIDDEN')))
    if (this.active >= this.maxConcurrent) return Promise.resolve(failedAiResult(new AiProviderError('AI_BUSY')))
    this.active += 1
    const promise = this.run(input).catch(failedAiResult).finally(() => {
      this.active -= 1
      if (this.runs.get(input.jobId)?.promise === promise) this.runs.delete(input.jobId)
    })
    this.runs.set(input.jobId, { fingerprint: fingerprint(input), promise })
    return promise
  }

  private async run(input: AiGenerationInput): Promise<AiGenerationResult> {
    await this.authorize(input)
    const references = []
    const hashes = new Set<string>()
    const warnings: string[] = []
    for (const id of input.materialIds) {
      const photo = this.options.store.photos.get(id)
      if (!photo) throw new AiProviderError('AI_MATERIAL_FORBIDDEN')
      let original: Buffer
      try {
        original = this.options.files.readOriginal
          ? await this.options.files.readOriginal(photo)
          : Buffer.from(photo.original)
      } catch {
        throw new AiProviderError('AI_REFERENCE_UNAVAILABLE')
      }
      if (!original.length) throw new AiProviderError('AI_REFERENCE_UNAVAILABLE')
      if (original.length > MAX_REFERENCE_BYTES) throw new AiProviderError('AI_REFERENCE_INVALID')
      const hash = digest(original)
      if (hashes.has(hash)) throw new AiProviderError('AI_MATERIAL_INVALID')
      hashes.add(hash)
      let reference: Awaited<ReturnType<typeof prepareReference>>
      try { reference = await prepareReference(original) } catch { throw new AiProviderError('AI_REFERENCE_INVALID') }
      if (Math.min(reference.width, reference.height) < 128) throw new AiProviderError('AI_REFERENCE_TOO_SMALL')
      warnings.push(...reference.warnings)
      hashes.add(digest(reference.bytes))
      references.push({ bytes: reference.bytes, mimeType: reference.mimeType })
    }
    // Consent can be revoked while originals are being read/decoded.
    await this.authorize(input)
    this.assertActive(input)
    const result = await this.options.imageProvider.generate({
      images: references,
      prompt: buildTogetherPrompt({ scene: this.options.scene ?? 'window' }),
      signal: input.signal,
    })
    this.assertActive(input)
    if (!result.bytes?.length || hashes.has(digest(result.bytes))) throw new AiProviderError('AI_RESULT_UNAVAILABLE')
    let rendered: Awaited<ReturnType<typeof renderPhoto>>
    const seed = createHash('sha256').update(input.jobId).digest().readUInt32BE(0)
    try { rendered = await renderPhoto(result.bytes, { presetId: this.resultPresetId, intensity: this.resultIntensity, seed, aiGenerated: true }) } catch { throw new AiProviderError('AI_RESULT_UNAVAILABLE') }
    const photo: Photo = {
      id: `p_ai_draft_${randomUUID()}`, authorId: input.actorId, filterId: this.resultPresetId, playType: 'template',
      beauty: 0, sticker: 'none', caption: null, circle: '小圈',
      width: rendered.metadata.width, height: rendered.metadata.height, createdAt: new Date().toISOString(),
      original: rendered.original, processed: rendered.web, draftJobId: input.jobId, aiGenerated: true,
      generation: {
        provider: this.name, model: result.model || this.model, promptVersion: PROMPT_VERSION,
        ...(result.requestId ? { requestId: result.requestId } : {}), referenceWarnings: [...new Set(warnings)],
        presetVersion: rendered.metadata.presetVersion, intensity: this.resultIntensity, seed,
      },
    }
    try { await this.options.files.save(photo) } catch {
      await this.options.files.remove(photo).catch(() => undefined)
      throw new AiProviderError('STORAGE_UNAVAILABLE')
    }
    if (input.signal?.aborted) {
      await this.options.files.remove(photo).catch(() => undefined)
      throw new AiProviderError('AI_CANCELLED')
    }
    this.options.store.photos.set(photo.id, photo)
    return { status: 'completed', resultPhotoId: photo.id }
  }

  async discard(input: AiGenerationInput): Promise<void> {
    const previous = this.runs.get(input.jobId)
    if (previous && previous.fingerprint !== fingerprint(input)) throw new AiProviderError('AI_MATERIAL_FORBIDDEN')
    // The route aborts first; run checks the signal again before storing a result.
    for (const photo of this.options.store.photos.values()) {
      if (photo.draftJobId !== input.jobId || photo.authorId !== input.actorId) continue
      try { await this.options.files.remove(photo) } catch { throw new AiProviderError('STORAGE_UNAVAILABLE') }
      this.options.store.photos.delete(photo.id)
    }
    this.runs.delete(input.jobId)
  }

  private assertActive(input: AiGenerationInput): void {
    if (input.signal?.aborted) throw new AiProviderError('AI_CANCELLED')
  }
}
