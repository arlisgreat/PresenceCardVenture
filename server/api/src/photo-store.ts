import { mkdir, readFile, writeFile, unlink } from 'node:fs/promises'
import path from 'node:path'
import type { Photo } from './demo-store.js'

export interface PhotoStorage {
  save(photo: Photo): Promise<void>
  read(photo: Photo): Promise<Buffer>
  readOriginal?(photo: Photo): Promise<Buffer>
  remove(photo: Photo): Promise<void>
}

export class PhotoStore implements PhotoStorage {
  constructor(private readonly dir: string) {}
  async save(photo: Photo) { const d = path.join(this.dir, photo.authorId); await mkdir(d, { recursive: true }); await writeFile(path.join(d, `${photo.id}-original.jpg`), photo.original); await writeFile(path.join(d, `${photo.id}-processed.jpg`), photo.processed) }
  async read(photo: Photo) { return readFile(path.join(this.dir, photo.authorId, `${photo.id}-processed.jpg`)).catch(error => { if ((error as NodeJS.ErrnoException).code === 'ENOENT' && photo.id.startsWith('p_demo_')) return photo.processed; throw error }) }
  // Legacy private storage key; decode the bytes, not its .jpg suffix. Originals may be PNG.
  async readOriginal(photo: Photo) { return readFile(path.join(this.dir, photo.authorId, `${photo.id}-original.jpg`)).catch(error => { if ((error as NodeJS.ErrnoException).code === 'ENOENT' && photo.id.startsWith('p_demo_')) return photo.original; throw error }) }
  async remove(photo: Photo) {
    const removeIfPresent = (file: string) => unlink(file).catch(error => {
      if ((error as NodeJS.ErrnoException).code !== 'ENOENT') throw error
    })
    await Promise.all([
      removeIfPresent(path.join(this.dir, photo.authorId, `${photo.id}-original.jpg`)),
      removeIfPresent(path.join(this.dir, photo.authorId, `${photo.id}-processed.jpg`)),
    ])
  }
}
