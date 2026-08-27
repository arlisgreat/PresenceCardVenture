export type AiGenerationInput = {
  materialIds: string[]
}

export type AiGenerationResult = {
  status: 'completed' | 'failed'
  resultPhotoId?: string
  error?: string
}

export interface AiProvider {
  readonly name: string
  generate(input: AiGenerationInput): Promise<AiGenerationResult>
}

/**
 * Deterministic local provider for the demo. It returns the first authorized
 * material as the preview result until a real image model is configured.
 */
export class SimulatorAiProvider implements AiProvider {
  readonly name = 'simulator'

  async generate(input: AiGenerationInput): Promise<AiGenerationResult> {
    if (input.materialIds.includes('fail')) return { status: 'failed', error: 'simulated generation failure' }
    return { status: 'completed', resultPhotoId: input.materialIds[0] }
  }
}
