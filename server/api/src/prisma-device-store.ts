import { createHash, randomBytes } from 'node:crypto'

export type DeviceRecord = {
  id: string
  pairCode?: string | null
  pairExpiresAt?: Date | null
  userId?: string | null
  tokenHash?: string | null
  fwVersion?: string | null
}

export type DeviceClient = {
  device: {
    upsert(args: { where: { id: string }; create: Record<string, unknown>; update: Record<string, unknown> }): Promise<DeviceRecord>
    findUnique(args: { where: { id: string } }): Promise<DeviceRecord | null>
    update(args: { where: { id: string }; data: Record<string, unknown> }): Promise<DeviceRecord>
  }
}

const hash = (value: string) => createHash('sha256').update(value).digest('hex')
const defaultToken = () => `device-${randomBytes(24).toString('base64url')}`

/** Prisma-backed pairing state. Plain device tokens are returned only at bind time. */
export class PrismaDeviceStore {
  readonly provider = 'prisma' as const

  constructor(
    private readonly client: DeviceClient,
    private readonly now: () => Date = () => new Date(),
    private readonly tokenFactory: () => string = defaultToken,
  ) {}

  async savePairCode(deviceId: string, pairCode: string, expiresAt: Date, fwVersion?: string): Promise<void> {
    const data = { pairCode, pairExpiresAt: expiresAt, ...(fwVersion ? { fwVersion } : {}) }
    await this.client.device.upsert({
      where: { id: deviceId },
      create: { id: deviceId, ...data },
      update: data,
    })
  }

  async bind(deviceId: string, pairCode: string, userId: string): Promise<{ deviceId: string; deviceToken: string }> {
    const device = await this.client.device.findUnique({ where: { id: deviceId } })
    if (!device || device.pairCode !== pairCode || !device.pairExpiresAt || device.pairExpiresAt.getTime() <= this.now().getTime()) {
      throw new Error('PAIR_EXPIRED')
    }
    const deviceToken = this.tokenFactory()
    await this.client.device.update({
      where: { id: deviceId },
      data: { userId, tokenHash: hash(deviceToken), pairCode: null, pairExpiresAt: null },
    })
    return { deviceId, deviceToken }
  }
}

