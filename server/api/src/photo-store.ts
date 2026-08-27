import { mkdir, writeFile, unlink } from 'node:fs/promises'
import path from 'node:path'
import type { Photo } from './demo-store.js'

export interface PhotoStorage {
  save(photo: Photo): Promise<void>
  remove(photo: Photo): Promise<void>
}

export class PhotoStore implements PhotoStorage {
  constructor(private readonly dir: string) {}
  async save(photo: Photo) { const d = path.join(this.dir, photo.authorId); await mkdir(d, { recursive: true }); await writeFile(path.join(d, `${photo.id}-original.jpg`), photo.original); await writeFile(path.join(d, `${photo.id}-processed.jpg`), photo.processed) }
  async remove(photo: Photo) { await Promise.all([unlink(path.join(this.dir, photo.authorId, `${photo.id}-original.jpg`)).catch(() => {}), unlink(path.join(this.dir, photo.authorId, `${photo.id}-processed.jpg`)).catch(() => {})]) }
}
