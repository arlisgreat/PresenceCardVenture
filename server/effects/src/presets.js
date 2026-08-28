/** Original Presence looks. Parameters operate on encoded sRGB, not linear scene light. */
export const PRESET_VERSION = 'presence-looks/1.0.0';

function freezePreset(preset) {
  for (const point of preset.toneCurve) Object.freeze(point);
  Object.freeze(preset.toneCurve);
  Object.freeze(preset.channelGain);
  Object.freeze(preset.shadowTint);
  Object.freeze(preset.highlightTint);
  Object.freeze(preset.glow.tint);
  Object.freeze(preset.glow);
  return Object.freeze({ version: PRESET_VERSION, ...preset });
}

export const PRESETS = Object.freeze([
  freezePreset({
    id: 'none', name: '原片', description: '保留拍摄时的颜色与纹理。', accent: '#fbf9f7',
    saturation: 1,
    toneCurve: [[0, 0], [1, 1]],
    channelGain: [1, 1, 1], shadowTint: [0, 0, 0], highlightTint: [0, 0, 0],
    grain: 0, vignette: 0,
    glow: { strength: 0, threshold: 0.78, tint: [1, 1, 1], radiusRatio: 0.006 },
  }),
  freezePreset({
    id: 'warm', name: '日光纸片', description: '柔暖高光、轻雾阴影，像窗边的一张生活照。', accent: '#f6dfe4',
    saturation: 0.94,
    toneCurve: [[0, 0.012], [0.12, 0.115], [0.35, 0.348], [0.60, 0.604], [0.82, 0.825], [1, 0.983]],
    channelGain: [1.008, 1, 0.990], shadowTint: [-0.002, 0.002, 0.009], highlightTint: [0.010, 0.003, -0.008],
    grain: 1.35, vignette: 0.035,
    glow: { strength: 0.065, threshold: 0.78, tint: [1, 0.72, 0.50], radiusRatio: 0.007 },
  }),
  freezePreset({
    id: 'bw', name: '银盐黑白', description: '克制的黑白层次与细颗粒，不改变面部结构。', accent: '#3a3f45',
    saturation: 0,
    toneCurve: [[0, 0.005], [0.12, 0.095], [0.35, 0.325], [0.60, 0.625], [0.82, 0.850], [1, 0.986]],
    channelGain: [1, 1, 1], shadowTint: [0, 0, 0], highlightTint: [0, 0, 0],
    grain: 2.20, vignette: 0.055,
    glow: { strength: 0.035, threshold: 0.80, tint: [1, 1, 1], radiusRatio: 0.006 },
  }),
  freezePreset({
    id: 'film', name: '即时相纸', description: '微褪色、淡粉阴影与细颗粒，保留真实肤色纹理。', accent: '#a8bfa0',
    saturation: 0.88,
    toneCurve: [[0, 0.030], [0.12, 0.126], [0.35, 0.342], [0.60, 0.590], [0.82, 0.805], [1, 0.965]],
    channelGain: [1.004, 1, 0.997], shadowTint: [0.006, 0.001, 0.009], highlightTint: [0.007, 0.003, -0.005],
    grain: 2.55, vignette: 0.040,
    glow: { strength: 0.045, threshold: 0.79, tint: [1, 0.85, 0.76], radiusRatio: 0.008 },
  }),
  freezePreset({
    id: 'vivid', name: '傍晚数码', description: '雾蓝暗部、微暖亮部与轻对比，保留小相机的直接感。', accent: '#dce8f2',
    saturation: 1.06,
    toneCurve: [[0, 0.004], [0.12, 0.105], [0.35, 0.331], [0.60, 0.612], [0.82, 0.845], [1, 0.989]],
    channelGain: [1.002, 1, 1.003], shadowTint: [-0.002, 0.003, 0.010], highlightTint: [0.008, 0.002, -0.003],
    grain: 0.90, vignette: 0.025,
    glow: { strength: 0.035, threshold: 0.82, tint: [1, 0.90, 0.80], radiusRatio: 0.005 },
  }),
]);

const byId = new Map(PRESETS.map((preset) => [preset.id, preset]));

export function getPreset(id) {
  const preset = byId.get(id);
  if (!preset) throw new RangeError(`Unknown Presence preset: ${String(id)}`);
  return preset;
}
