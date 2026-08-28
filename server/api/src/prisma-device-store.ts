import { createCipheriv, createDecipheriv, createHash, randomBytes } from 'node:crypto'

export type DeviceRecord = {
  id: string
  pairCode?: string | null
  pairExpiresAt?: Date | null
  userId?: string | null
  tokenHash?: string | null
  tokenCiphertext?: string | null
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
const encryptionKey = (secret: string) => createHash('sha256').update(secret).digest()
const encrypt = (value: string, secret: string) => {
  const iv = randomBytes(12)
  const cipher = createCipheriv('aes-256-gcm', encryptionKey(secret), iv)
  const ciphertext = Buffer.concat([cipher.update(value, 'utf8'), cipher.final()])
  return Buffer.concat([iv, cipher.getAuthTag(), ciphertext]).toString('base64url')
}
const decrypt = (value: string, secret: string) => {
  const payload = Buffer.from(value, 'base64url')
  const decipher = createDecipheriv('aes-256-gcm', encryptionKey(secret), payload.subarray(0, 12))
  decipher.setAuthTag(payload.subarray(12, 28))
  return Buffer.concat([decipher.update(payload.subarray(28)), decipher.final()]).toString('utf8')
}

/** Prisma-backed pairing state. Plain device tokens are returned only at bind time. */
export class PrismaDeviceStore {
  readonly provider = 'prisma' as const

  constructor(
    private readonly client: DeviceClient,
    private readonly now: () => Date = () => new Date(),
    private readonly tokenFactory: () => string = defaultToken,
    private readonly options: { encryptionKey?: string; requireEncryption?: boolean } = {},
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
    if (this.options.requireEncryption && !this.options.encryptionKey) throw new Error('DEVICE_TOKEN_KEY_MISSING')
    const deviceToken = this.tokenFactory()
    const tokenData = this.options.encryptionKey ? { tokenCiphertext: encrypt(deviceToken, this.options.encryptionKey) } : {}
    await this.client.device.update({
      where: { id: deviceId },
      data: { userId, tokenHash: hash(deviceToken), ...tokenData },
    })
    return { deviceId, deviceToken }
  }

  async status(deviceId: string, pairCode: string): Promise<{ status: 'pending' | 'bound'; deviceToken?: string; userId?: string }> {
    const device = await this.client.device.findUnique({ where: { id: deviceId } })
    if (!device || device.pairCode !== pairCode || !device.pairExpiresAt || device.pairExpiresAt.getTime() <= this.now().getTime()) throw new Error('PAIR_EXPIRED')
    if (!device.userId) return { status: 'pending' }
    if (!device.tokenCiphertext) return { status: 'bound', userId: device.userId }
    if (!this.options.encryptionKey) throw new Error('DEVICE_TOKEN_KEY_MISSING')
    return { status: 'bound', userId: device.userId, deviceToken: decrypt(device.tokenCiphertext, this.options.encryptionKey) }
  }
}
