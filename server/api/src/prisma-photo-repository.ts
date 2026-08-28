import type { Photo } from './demo-store.js'

export type PhotoClient = {
  photo: {
    create(args: any): Promise<any>
    findUnique(args: any): Promise<any | null>
    delete(args: any): Promise<any>
  }
  idempotencyKey: {
    findUnique(args: any): Promise<any>
  }
}

export interface PhotoMetadataRepository {
  readonly provider: string
  readonly complete?: boolean
  create(photo: Photo): Promise<void>
  findById(id: string): Promise<Photo | undefined>
  findByIdempotency(authorId: string, deviceId: string, key: string): Promise<Photo | undefined>
  remove(id: string): Promise<void>
}

const mapPhoto = (record: any): Photo | undefined => {
  if (!record) return undefined
  return {
    id: String(record.id),
    authorId: String(record.authorId),
    filterId: String(record.filterId ?? 'none'),
    playType: String(record.playType ?? 'ccd'),
    beauty: Number(record.beauty ?? 0),
    sticker: String(record.sticker ?? 'none'),
    caption: record.caption == null ? null : String(record.caption),
    circle: String(record.circle ?? '小圈'),
    width: Number(record.width),
    height: Number(record.height),
    createdAt: new Date(record.createdAt).toISOString(),
    original: Buffer.alloc(0),
    processed: Buffer.alloc(0),
  }
}

export class PrismaPhotoRepository implements PhotoMetadataRepository {
  readonly provider = 'prisma' as const
  readonly complete = false

  constructor(private readonly client: PhotoClient) {}

  async create(photo: Photo): Promise<void> {
    await this.client.photo.create({
      data: {
        id: photo.id,
        authorId: photo.authorId,
        ossKey: `photos/${photo.authorId}/${photo.id}.jpg`,
        originalOssKey: `photos/${photo.authorId}/${photo.id}-original.jpg`,
        filterId: photo.filterId,
        playType: photo.playType ?? 'ccd',
        beauty: photo.beauty ?? 0,
        sticker: photo.sticker ?? 'none',
        circle: photo.circle ?? '小圈',
        caption: photo.caption,
        width: photo.width,
        height: photo.height,
        sizeBytes: photo.processed.length,
        createdAt: new Date(photo.createdAt),
      },
    })
  }

  async findById(id: string): Promise<Photo | undefined> {
    return mapPhoto(await this.client.photo.findUnique({ where: { id } }))
  }

  async findByIdempotency(authorId: string, deviceId: string, key: string): Promise<Photo | undefined> {
    const result = await this.client.idempotencyKey.findUnique({
      where: { deviceId_key: { deviceId, key } },
      include: { photo: true },
    })
    const photo = mapPhoto(result?.photo)
    return photo?.authorId === authorId ? photo : undefined
  }

  async remove(id: string): Promise<void> {
    await this.client.photo.delete({ where: { id } })
  }
}
