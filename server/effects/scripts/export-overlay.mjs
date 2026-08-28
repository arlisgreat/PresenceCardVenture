import { parseArgs } from 'node:util';
import { createHash } from 'node:crypto';
import { open, mkdir, unlink } from 'node:fs/promises';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import sharp from 'sharp';

export const OVERLAY_LIMITS = Object.freeze({ inputBytes: 10 * 1024 * 1024, inputPixels: 16_000_000, edge: 128 });
const PNG_SIGNATURE = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
const RESERVED = new Set(('alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept auto bitand bitor bool break case catch char char8_t char16_t char32_t class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public register reinterpret_cast requires restrict return short signed sizeof static static_assert static_cast struct switch synchronized template this thread_local throw true try typedef typeid typename typeof typeof_unqual union unsigned using virtual void volatile wchar_t while xor xor_eq').split(' '));

function validateSymbol(symbol) {
  if (typeof symbol !== 'string' || !/^[A-Za-z][A-Za-z0-9_]{0,63}$/.test(symbol) || symbol.includes('__') || RESERVED.has(symbol)) {
    throw new Error('symbol must be a non-reserved C/C++ identifier, 1..64 characters, starting with a letter');
  }
}

function validatePng(bytes) {
  if (!bytes.length || bytes.length > OVERLAY_LIMITS.inputBytes) throw new Error('PNG input must be 1 byte..10 MiB');
  if (bytes.length < 8 || !bytes.subarray(0, 8).equals(PNG_SIGNATURE)) throw new Error('Only static PNG input is supported, not SVG or other formats');
  let offset = 8, hasData = false;
  while (offset + 12 <= bytes.length) {
    const length = bytes.readUInt32BE(offset);
    if (length > bytes.length - offset - 12) throw new Error('Invalid or truncated PNG');
    const type = bytes.toString('latin1', offset + 4, offset + 8);
    if (!/^[A-Za-z]{4}$/.test(type)) throw new Error('Invalid PNG chunk');
    if (offset === 8) {
      if (type !== 'IHDR' || length !== 13) throw new Error('Invalid PNG header');
      const width = bytes.readUInt32BE(offset + 8), height = bytes.readUInt32BE(offset + 12);
      if (!width || !height || width > OVERLAY_LIMITS.inputPixels / height) throw new Error('PNG exceeds 16 megapixels or has invalid dimensions');
    } else if (type === 'IHDR') throw new Error('Invalid duplicate PNG header');
    if (['acTL', 'fcTL', 'fdAT'].includes(type)) throw new Error('Animated PNG is not supported');
    if (type === 'IDAT') hasData = true;
    offset += length + 12;
    if (type === 'IEND') {
      if (length !== 0 || offset !== bytes.length || !hasData) throw new Error('Invalid PNG end');
      return;
    }
  }
  throw new Error('Invalid or truncated PNG');
}

function cArray(type, name, values, encode) {
  const lines = [`static const ${type} ${name}[${values.length}] = {`];
  for (let offset = 0; offset < values.length; offset += 12) lines.push(`  ${Array.from(values.subarray(offset, offset + 12), encode).join(', ')},`);
  lines.push('};');
  return lines.join('\n');
}

/** Offline conversion only; license/ownership cannot be inferred from PNG bytes. */
export async function createOverlayAsset(input, { symbol, sourceName } = {}) {
  validateSymbol(symbol);
  if (!(input instanceof Uint8Array)) throw new Error('PNG input bytes required');
  if (input.byteLength > OVERLAY_LIMITS.inputBytes) throw new Error('PNG input exceeds 10 MiB');
  const bytes = Buffer.from(input);
  validatePng(bytes);
  let decoded;
  try {
    decoded = await sharp(bytes, { limitInputPixels: OVERLAY_LIMITS.inputPixels, failOn: 'error' })
      .rotate().toColourspace('srgb').ensureAlpha()
      .resize({ width: OVERLAY_LIMITS.edge, height: OVERLAY_LIMITS.edge, fit: 'inside', withoutEnlargement: true })
      .raw({ depth: 'uchar' }).toBuffer({ resolveWithObject: true });
  } catch { throw new Error('PNG cannot be decoded'); }
  const { data, info } = decoded;
  if (!info.width || !info.height || info.width > OVERLAY_LIMITS.edge || info.height > OVERLAY_LIMITS.edge || info.channels !== 4 || data.length !== info.width * info.height * 4) {
    throw new Error('Invalid decoded overlay');
  }
  const count = info.width * info.height, rgb565 = new Uint16Array(count), alpha = new Uint8Array(count);
  for (let index = 0; index < count; index++) {
    rgb565[index] = (Math.round(data[index * 4] * 31 / 255) << 11) |
      (Math.round(data[index * 4 + 1] * 63 / 255) << 5) | Math.round(data[index * 4 + 2] * 31 / 255);
    alpha[index] = data[index * 4 + 3];
  }
  const sha256 = createHash('sha256').update(bytes).digest('hex');
  const guard = `PRESENCE_ASSET_${symbol}_H`;
  const header = [
    `#ifndef ${guard}`, `#define ${guard}`, '#include <stdint.h>', '',
    '/* Numeric RGB565 words; camera/display byte order is chosen by firmware.',
    ' * Separate straight alpha; blend in encoded RGB565 channels, NOT linear-light.',
    ' * Row-major, top to bottom. Static storage placement is linker-dependent.',
    ` * Source SHA-256: ${sha256}`,
    ' * Rights are not inferred: see the companion provenance JSON. */',
    `enum { ${symbol}_width = ${info.width}, ${symbol}_height = ${info.height},`,
    `       ${symbol}_rgb565_words = ${count}, ${symbol}_alpha_bytes = ${count} };`, '',
    cArray('uint16_t', `${symbol}_rgb565`, rgb565, value => `0x${value.toString(16).padStart(4, '0')}`), '',
    cArray('uint8_t', `${symbol}_alpha`, alpha, value => String(value)), '', `#endif /* ${guard} */`, '',
  ].join('\n');
  const provenance = {
    format: 'presence-overlay-v1', symbol, sourceSha256: sha256,
    ...(typeof sourceName === 'string' ? { sourceFile: path.basename(sourceName) } : {}),
    sourceBytes: bytes.length, width: info.width, height: info.height,
    rgb565Words: count, alphaBytes: count, payloadBytes: count * 3,
    order: 'row-major-top-to-bottom', rgb565: 'R[15:11],G[10:5],B[4:0]',
    byteOrder: 'numeric-words; firmware selects camera/display byte order',
    alpha: 'straight-uint8', blend: 'encoded-rgb565-rounded; not-linear-light',
    conversion: 'auto-orient; sRGB; fit within 128x128 without enlargement; nearest 5/6/5 channel quantization',
    license: 'not-inferred',
    rights: 'Operator must own or hold a license allowing embedding and redistribution in the intended product; conversion grants no rights.',
  };
  return { width: info.width, height: info.height, rgb565, alpha, header, provenance };
}

async function readBoundedFile(inputPath) {
  const file = await open(inputPath, 'r');
  try {
    const stat = await file.stat();
    if (!stat.isFile()) throw new Error('PNG input must be a regular file');
    if (!stat.size || stat.size > OVERLAY_LIMITS.inputBytes) throw new Error('PNG input must be 1 byte..10 MiB');
    const chunks = [];
    let total = 0;
    while (true) {
      const chunk = Buffer.allocUnsafe(Math.min(65536, OVERLAY_LIMITS.inputBytes + 1 - total));
      const { bytesRead } = await file.read(chunk, 0, chunk.length, null);
      if (!bytesRead) break;
      total += bytesRead;
      if (total > OVERLAY_LIMITS.inputBytes) throw new Error('PNG input exceeds 10 MiB');
      chunks.push(chunk.subarray(0, bytesRead));
    }
    return Buffer.concat(chunks, total);
  } finally { await file.close(); }
}

export async function exportOverlay({ input, out, symbol }) {
  validateSymbol(symbol);
  if (typeof input !== 'string' || !input || typeof out !== 'string' || !out) throw new Error('input and out paths are required');
  const asset = await createOverlayAsset(await readBoundedFile(input), { symbol, sourceName: input });
  const directory = path.resolve(out), headerPath = path.join(directory, `${symbol}.h`), provenancePath = path.join(directory, `${symbol}.json`);
  await mkdir(directory, { recursive: true });
  const created = [];
  try {
    // Reserve both names before writing; wx also rejects existing symlinks.
    for (const target of [headerPath, provenancePath]) created.push({ path: target, handle: await open(target, 'wx', 0o600) });
    await created[0].handle.writeFile(asset.header, 'utf8');
    await created[1].handle.writeFile(`${JSON.stringify(asset.provenance, null, 2)}\n`, 'utf8');
  } catch (error) {
    for (const item of created) {
      await item.handle.close().catch(() => {});
      await unlink(item.path).catch(() => {}); // Only files created by this invocation.
    }
    throw error;
  } finally { for (const item of created) await item.handle.close().catch(() => {}); }
  return { header: headerPath, provenance: provenancePath, width: asset.width, height: asset.height, payloadBytes: asset.provenance.payloadBytes };
}

export async function main(args = process.argv.slice(2)) {
  const { values } = parseArgs({ args, options: { input: { type: 'string' }, out: { type: 'string' }, symbol: { type: 'string' }, help: { type: 'boolean' } } });
  const usage = 'Usage: node scripts/export-overlay.mjs --input owned.png --out output/overlays --symbol presence_sticker';
  if (values.help) { console.log(usage); return; }
  if (!values.input || !values.out || !values.symbol) throw new Error(usage);
  console.log(JSON.stringify(await exportOverlay(values)));
}

if (process.argv[1] && pathToFileURL(path.resolve(process.argv[1])).href === import.meta.url) {
  main().catch(error => { console.error(error.message); process.exitCode = 1; });
}
