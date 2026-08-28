import sharp from 'sharp';
import { createHash } from 'node:crypto';
import { applyLookToRgba, PRESET_VERSION } from './pixels.js';
import { getPreset } from './presets.js';

export const LIMITS = Object.freeze({ inputBytes: 10 * 1024 * 1024, inputPixels: 20_000_000, webEdge: 1600, deviceBytes: 100 * 1024 });
export class EffectsError extends Error {
  constructor(code, message) { super(message); this.name = 'EffectsError'; this.code = code; }
}

export function detectMime(bytes) {
  if (!(bytes instanceof Uint8Array)) throw new EffectsError('INVALID_IMAGE', 'Image bytes required');
  if (bytes.length >= 3 && bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff) return 'image/jpeg';
  if (bytes.length >= 8 && Buffer.from(bytes.subarray(0, 8)).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]))) return 'image/png';
  if (bytes.length >= 12 && Buffer.from(bytes.subarray(0, 4)).toString('ascii') === 'RIFF' && Buffer.from(bytes.subarray(8, 12)).toString('ascii') === 'WEBP') return 'image/webp';
  throw new EffectsError('UNSUPPORTED_IMAGE', 'Only JPEG, PNG and static WebP are supported');
}

async function inspect(input) {
  if (!(input instanceof Uint8Array) || !input.length) throw new EffectsError('INVALID_IMAGE', 'Image bytes required');
  if (input.length > LIMITS.inputBytes) throw new EffectsError('INPUT_TOO_LARGE', 'Image exceeds 10 MiB');
  const mimeType = detectMime(input);
  const source = Buffer.from(input);
  let metadata;
  try { metadata = await sharp(source, { limitInputPixels: LIMITS.inputPixels, failOn: 'error' }).metadata(); }
  catch { throw new EffectsError('INVALID_IMAGE', 'Image cannot be decoded or exceeds the pixel limit'); }
  if (!metadata.width || !metadata.height || metadata.width * metadata.height > LIMITS.inputPixels) throw new EffectsError('IMAGE_TOO_LARGE', 'Image exceeds 20 megapixels');
  if ((metadata.pages ?? 1) > 1) throw new EffectsError('UNSUPPORTED_IMAGE', 'Animated images are not supported');
  return { source, metadata, mimeType };
}

function decoded(source) {
  return sharp(source, { limitInputPixels: LIMITS.inputPixels, failOn: 'error' }).rotate().toColourspace('srgb');
}

/** Strips EXIF/GPS from a reference, never fabricates extra detail by upscaling. */
export async function prepareReference(input) {
  const { source } = await inspect(input);
  try {
    const { data, info } = await decoded(source).resize({ width: 1600, height: 1600, fit: 'inside', withoutEnlargement: true })
      .flatten({ background: '#fbf9f7' }).jpeg({ quality: 92, chromaSubsampling: '4:4:4', progressive: false }).toBuffer({ resolveWithObject: true });
    return { bytes: data, mimeType: 'image/jpeg', width: info.width, height: info.height,
      warnings: Math.min(info.width, info.height) < 384 ? ['LOW_RESOLUTION_REFERENCE'] : [] };
  } catch (error) {
    if (error instanceof EffectsError) throw error;
    throw new EffectsError('INVALID_IMAGE', 'Image cannot be decoded');
  }
}

/** Original remains private and byte-exact; both delivery derivatives are metadata-free JPEG. */
export async function renderPhoto(input, options = {}) {
  const { presetId = 'none', intensity = 1, seed = 1, webMaxEdge = LIMITS.webEdge, aiGenerated = false } = options;
  getPreset(presetId);
  if (!Number.isInteger(webMaxEdge) || webMaxEdge < 320 || webMaxEdge > 2048) throw new EffectsError('INVALID_OPTIONS', 'webMaxEdge must be 320..2048');
  if (!Number.isFinite(intensity) || intensity < 0 || intensity > 1 || !Number.isSafeInteger(seed)) throw new EffectsError('INVALID_OPTIONS', 'Invalid intensity or seed');
  const { source, metadata: sourceInfo, mimeType } = await inspect(input);
  try {
    const { data, info } = await decoded(source).resize({ width: webMaxEdge, height: webMaxEdge, fit: 'inside', withoutEnlargement: true })
      .flatten({ background: '#fbf9f7' }).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
    const pixels = applyLookToRgba({ data, width: info.width, height: info.height }, { presetId, intensity, seed });
    const raster = () => sharp(Buffer.from(pixels), { raw: { width: info.width, height: info.height, channels: 4 } });
    const webRaster = raster();
    if (aiGenerated) webRaster.composite([{ input: aiLabel(info.width, info.height), gravity: 'southeast' }]);
    const web = await webRaster.jpeg({ quality: 90, chromaSubsampling: '4:4:4', progressive: false }).toBuffer();
    // Fit before padding, so portrait faces are never cropped and small sources are never upscaled.
    const deviceRaster = await raster().resize({ width: 320, height: 240, fit: 'inside', withoutEnlargement: true }).png().toBuffer();
    let device, deviceQuality;
    for (const quality of [88, 80, 70, 60, 48]) {
      const deviceEncoder = sharp(deviceRaster).resize({ width: 320, height: 240, fit: 'contain', background: '#fbf9f7', withoutEnlargement: true });
      if (aiGenerated) deviceEncoder.composite([{ input: aiLabel(320, 240), gravity: 'southeast' }]);
      device = await deviceEncoder.jpeg({ quality, chromaSubsampling: '4:2:0', progressive: false }).toBuffer();
      deviceQuality = quality;
      if (device.length <= LIMITS.deviceBytes) break;
    }
    if (device.length > LIMITS.deviceBytes) throw new EffectsError('DEVICE_IMAGE_TOO_LARGE', 'Device JPEG exceeds 100 KiB');
    return {
      original: source, web, device,
      metadata: {
        presetId, presetVersion: PRESET_VERSION, intensity, seed, sourceMimeType: mimeType,
        sourceWidth: sourceInfo.width, sourceHeight: sourceInfo.height,
        width: info.width, height: info.height, deviceWidth: 320, deviceHeight: 240, deviceQuality,
        sha256: createHash('sha256').update(source).digest('hex'),
        bytes: { original: source.length, web: web.length, device: device.length },
        warnings: Math.min(info.width, info.height) < 384 ? ['LOW_RESOLUTION_SOURCE'] : [],
      },
    };
  } catch (error) {
    if (error instanceof EffectsError || error instanceof RangeError || error instanceof TypeError) throw error;
    throw new EffectsError('INVALID_IMAGE', 'Image processing failed');
  }
}

// A visible disclosure survives metadata stripping and standalone card display.
function aiLabel(width, height) {
  const scale = Math.max(1, Math.min(width, height) / 480);
  const w = Math.min(width, Math.round(40 * scale)), h = Math.min(height, Math.round(26 * scale));
  // Vector lettering stays visible in minimal containers without installed fonts.
  return Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 40 26"><rect x="2" y="2" width="36" height="22" rx="3" fill="#fbf9f7" fill-opacity="0.88"/><path d="M10 18L15 7L20 18M12 14H18M24 7H30M27 7V18M24 18H30" fill="none" stroke="#3a3f45" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/></svg>`);
}

/** Deliberately non-generative alternative: two actual photos on one paper card. */
export async function createPhotoPair(inputs, options = {}) {
  if (!Array.isArray(inputs) || inputs.length !== 2) throw new EffectsError('INVALID_OPTIONS', 'Exactly two images required');
  const tiles = [];
  for (const input of inputs) {
    const rendered = await renderPhoto(input, options);
    tiles.push(await sharp(rendered.web).resize({ width: 456, height: 570, fit: 'contain', background: '#fbf9f7' }).toBuffer());
  }
  const paired = await sharp({ create: { width: 1024, height: 768, channels: 3, background: '#fbf9f7' } })
    .composite([{ input: tiles[0], left: 40, top: 48 }, { input: tiles[1], left: 528, top: 48 }])
    .jpeg({ quality: 92, progressive: false }).toBuffer();
  const output = await renderPhoto(paired, { presetId: 'none' });
  return { ...output, kind: 'photo-pair', aiGenerated: false };
}
