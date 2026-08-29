export type AiGenerationInput = {
  materialIds: string[]
  actorId: string
  jobId: string
  signal?: AbortSignal
}

export type AiGenerationResult = {
  status: 'completed' | 'failed'
  resultPhotoId?: string
  error?: string
  errorCode?: AiErrorCode
}

const AI_ERRORS = {
  AI_MATERIAL_INVALID: [400, 'Choose two different image materials.'],
  AI_MATERIAL_FORBIDDEN: [403, 'Material consent is missing or no longer valid.'],
  AI_CONSENT_UNAVAILABLE: [503, 'Material consent cannot be verified right now.'],
  AI_REFERENCE_UNAVAILABLE: [409, 'An original image is unavailable. Upload it again.'],
  AI_REFERENCE_TOO_SMALL: [422, 'A reference image is too small. Upload a larger photo.'],
  AI_REFERENCE_INVALID: [422, 'A reference image cannot be used. Upload a valid image.'],
  AI_BUSY: [429, 'Image generation is busy. Please try again later.'],
  AI_CANCELLED: [409, 'Image generation was cancelled.'],
  AI_GENERATION_FAILED: [502, 'Image generation failed. Please try again later.'],
  AI_PROVIDER_TIMEOUT: [504, 'The image service timed out. Please try again later.'],
  AI_PROVIDER_REJECTED: [422, 'The image service could not process these materials.'],
  AI_RESULT_UNAVAILABLE: [409, 'The generated image is unavailable.'],
  STORAGE_UNAVAILABLE: [503, 'Image storage is unavailable. Please try again later.'],
} as const

export type AiErrorCode = keyof typeof AI_ERRORS

/** Only these messages may cross the API boundary; never forward provider bodies. */
export class AiProviderError extends Error {
  readonly statusCode: number

  constructor(readonly code: AiErrorCode) {
    super(AI_ERRORS[code][1])
    this.statusCode = AI_ERRORS[code][0]
  }
}

export function safeAiError(error: unknown): AiProviderError {
  if (error instanceof AiProviderError) return new AiProviderError(error.code)
  const code = typeof error === 'object' && error !== null && 'code' in error ? String(error.code) : ''
  if (code === 'PROVIDER_TIMEOUT') return new AiProviderError('AI_PROVIDER_TIMEOUT')
  if (code === 'PROVIDER_RATE_LIMITED') return new AiProviderError('AI_BUSY')
  if (code === 'PROVIDER_REJECTED') return new AiProviderError('AI_PROVIDER_REJECTED')
  return new AiProviderError('AI_GENERATION_FAILED')
}

export function failedAiResult(error: unknown): AiGenerationResult {
  const safe = safeAiError(error)
  return { status: 'failed', errorCode: safe.code, error: safe.message }
}

export interface AiProvider {
  readonly name: string
  authorize?(input: AiGenerationInput, purpose?: 'generate' | 'publish'): Promise<void>
  generate(input: AiGenerationInput): Promise<AiGenerationResult>
  discard?(input: AiGenerationInput): Promise<void>
}

/**
 * Deterministic local provider for the demo. It returns the first authorized
 * material as the preview result until a real image model is configured.
 */
export class SimulatorAiProvider implements AiProvider {
  readonly name = 'simulator'

  async generate(input: AiGenerationInput): Promise<AiGenerationResult> {
    if (input.materialIds.includes('fail')) return failedAiResult(new AiProviderError('AI_GENERATION_FAILED'))
    return { status: 'completed', resultPhotoId: input.materialIds[0] }
  }
}
