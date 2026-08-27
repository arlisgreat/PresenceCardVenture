import { mkdir, readFile, writeFile, unlink } from 'node:fs/promises'
import path from 'node:path'
import type { Photo } from './demo-store.js'

export interface PhotoStorage {
  save(photo: Photo): Promise<void>
  read(photo: Photo): Promise<Buffer>
  remove(photo: Photo): Promise<void>
}

export class PhotoStore implements PhotoStorage {
  constructor(private readonly dir: string) {}
  async save(photo: Photo) { const d = path.join(this.dir, photo.authorId); await mkdir(d, { recursive: true }); await writeFile(path.join(d, `${photo.id}-original.jpg`), photo.original); await writeFile(path.join(d, `${photo.id}-processed.jpg`), photo.processed) }
  async read(photo: Photo) { return readFile(path.join(this.dir, photo.authorId, `${photo.id}-processed.jpg`)).catch(error => { if ((error as NodeJS.ErrnoException).code === 'ENOENT' && photo.id.startsWith('p_demo_')) return photo.processed; throw error }) }
  async remove(photo: Photo) { await Promise.all([unlink(path.join(this.dir, photo.authorId, `${photo.id}-original.jpg`)).catch(() => {}), unlink(path.join(this.dir, photo.authorId, `${photo.id}-processed.jpg`)).catch(() => {})]) }
}
