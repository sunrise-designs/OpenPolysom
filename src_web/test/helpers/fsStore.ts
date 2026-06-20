import { readFile } from 'node:fs/promises';
import { join } from 'node:path';

/**
 * Minimal filesystem-backed zarrita `Readable` store for tests — reads chunk /
 * metadata bytes from a local Zarr directory so the read-parity test needs no
 * HTTP server. Satisfies the structural `{ get(key): Promise<Uint8Array | undefined> }`
 * contract zarrita's stores expose.
 */
export class FsStore {
  readonly #root: string;

  constructor(root: string) {
    this.#root = root;
  }

  async get(key: string): Promise<Uint8Array | undefined> {
    try {
      const buf = await readFile(join(this.#root, key));
      return new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
    } catch (err) {
      if ((err as NodeJS.ErrnoException).code === 'ENOENT') return undefined;
      throw err;
    }
  }
}
