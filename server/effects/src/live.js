/**
 * Original vector stickers driven by an already-authorized MediaPipe FaceLandmarker
 * result. No detector, model download, camera access or network calls live here.
 * The caller must explicitly accept the detector's tracking result. A measured
 * confidence is optional; none is fabricated when the detector does not expose it.
 */
export const FACE_OVERLAY_VERSION = 'presence-face-overlays/1.0.0';

// MediaPipe FaceLandmarker 478-point topology; not an arbitrary landmark format.
// These anchors are used only for 2D placement/scale, never identity recognition.
export const FACE_ANCHORS = Object.freeze({ eyeA: 33, eyeB: 263, nose: 1, sideA: 234, sideB: 454 });

const clamp = (value, low, high) => Math.min(high, Math.max(low, value));
const mix = (a, b, alpha) => a + (b - a) * alpha;
const finite = Number.isFinite;
const styles = new Set(['cheek-stars', 'orbit']);
const hexColor = /^#[0-9a-f]{6}$/i;

function pixelPoint(landmarks, index, width, height) {
  const point = landmarks[index];
  if (!point || !finite(point.x) || !finite(point.y) || (point.z !== undefined && !finite(point.z))) return null;
  return { x: clamp(point.x, 0, 1) * width, y: clamp(point.y, 0, 1) * height };
}

function boundedStar(command, width, height) {
  const radius = Math.min(command.radius, width / 2, height / 2);
  return {
    ...command,
    x: clamp(command.x, radius, width - radius),
    y: clamp(command.y, radius, height - radius),
    radius,
    innerRadius: Math.min(command.innerRadius, radius),
  };
}

function boundedEllipse(command, width, height) {
  const cos = Math.cos(command.rotation);
  const sin = Math.sin(command.rotation);
  const halfStroke = command.lineWidth / 2;
  const extentX = Math.hypot(command.radiusX * cos, command.radiusY * sin) + halfStroke;
  const extentY = Math.hypot(command.radiusX * sin, command.radiusY * cos) + halfStroke;
  return {
    ...command,
    x: clamp(command.x, extentX, width - extentX),
    y: clamp(command.y, extentY, height - extentY),
  };
}

/**
 * Build small original vector overlays. Missing/low confidence, missing faces,
 * wrong topology, invalid anchors or degenerate geometry produce no commands.
 * Mirroring happens once, at output: pass the unmirrored detector coordinates.
 */
export function buildFaceOverlays(landmarks, {
  width, height, style = 'cheek-stars', mirrored = false,
  trackingAccepted = false, confidence, minConfidence = 0.65,
} = {}) {
  if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height) || width < 8 || height < 8 || width > 32768 || height > 32768) return [];
  if (!styles.has(style) || typeof mirrored !== 'boolean') return [];
  if (trackingAccepted !== true || !finite(minConfidence) || minConfidence < 0 || minConfidence > 1) return [];
  if (confidence !== undefined && (!finite(confidence) || confidence < 0 || confidence > 1 || confidence < minConfidence)) return [];
  if (!Array.isArray(landmarks) || landmarks.length !== 478) return [];
  const anchors = Object.fromEntries(Object.entries(FACE_ANCHORS).map(([name, index]) => [name, pixelPoint(landmarks, index, width, height)]));
  if (Object.values(anchors).some((point) => !point)) return [];
  const { eyeA, eyeB, nose, sideA, sideB } = anchors;
  const eyeSpan = Math.hypot(eyeB.x - eyeA.x, eyeB.y - eyeA.y);
  const sideSpan = Math.hypot(sideB.x - sideA.x, sideB.y - sideA.y);
  if (eyeSpan < Math.max(4, Math.min(width, height) * 0.025) || sideSpan < eyeSpan * 0.75) return [];
  const center = { x: (eyeA.x + eyeB.x) / 2, y: (eyeA.y + eyeB.y) / 2 };
  let roll = Math.atan2(eyeB.y - eyeA.y, eyeB.x - eyeA.x);
  if (roll > Math.PI / 2) roll -= Math.PI;
  if (roll < -Math.PI / 2) roll += Math.PI;
  const axis = { x: Math.cos(roll), y: Math.sin(roll) };
  const down = { x: -axis.y, y: axis.x };
  const radius = clamp(eyeSpan * 0.055, 1.2, Math.min(width, height) * 0.035);
  const commands = [];

  if (style === 'cheek-stars') {
    for (const [name, anchor, direction] of [['a', sideA, -1], ['b', sideB, 1]]) {
      const x = mix(anchor.x, nose.x, 0.40) + down.x * eyeSpan * 0.055;
      const y = mix(anchor.y, nose.y, 0.40) + down.y * eyeSpan * 0.055;
      commands.push(boundedStar({
        type: 'star', id: `cheek-${name}-main`, x, y,
        radius, innerRadius: radius * 0.30, points: 4,
        rotation: roll + direction * 0.12, opacity: 0.84, fill: '#d98ba0',
      }, width, height));
      commands.push(boundedStar({
        type: 'star', id: `cheek-${name}-small`,
        x: x + axis.x * radius * direction * 1.65 - down.x * radius * 1.25,
        y: y + axis.y * radius * direction * 1.65 - down.y * radius * 1.25,
        radius: radius * 0.48, innerRadius: radius * 0.14, points: 4,
        rotation: roll - direction * 0.18, opacity: 0.70, fill: '#fbf9f7',
      }, width, height));
    }
  } else {
    const maxRadius = Math.min(width, height) * 0.44;
    const radiusX = Math.min(eyeSpan * 0.88, maxRadius);
    const radiusY = Math.min(eyeSpan * 0.34, maxRadius);
    const ellipse = boundedEllipse({
      type: 'ellipse', id: 'orbit-ring', x: center.x, y: center.y - eyeSpan * 0.30,
      radiusX, radiusY, rotation: roll - 0.16, opacity: 0.24,
      stroke: '#7fa3c4', lineWidth: clamp(eyeSpan * 0.006, 0.6, 1.6),
    }, width, height);
    commands.push(ellipse);
    for (let i = 0; i < 5; i++) {
      const angle = -Math.PI * 0.84 + i * Math.PI * 0.40;
      const localX = Math.cos(angle) * radiusX;
      const localY = Math.sin(angle) * radiusY;
      commands.push(boundedStar({
        type: 'star', id: `orbit-star-${i}`,
        x: ellipse.x + localX * Math.cos(ellipse.rotation) - localY * Math.sin(ellipse.rotation),
        y: ellipse.y + localX * Math.sin(ellipse.rotation) + localY * Math.cos(ellipse.rotation),
        radius: radius * (i % 2 ? 0.60 : 0.90),
        innerRadius: radius * (i % 2 ? 0.18 : 0.27), points: 4,
        rotation: roll + angle * 0.20, opacity: i % 2 ? 0.62 : 0.80,
        fill: i % 2 ? '#a8bfa0' : '#d98ba0',
      }, width, height));
    }
  }
  if (!mirrored) return commands;
  return commands.map((command) => ({ ...command, x: width - command.x, rotation: -command.rotation }));
}

function isDrawCommand(command) {
  if (!command || typeof command !== 'object' || typeof command.id !== 'string') return false;
  if (![command.x, command.y, command.rotation, command.opacity].every(finite) || command.opacity < 0 || command.opacity > 1) return false;
  if (Math.abs(command.x) > 65536 || Math.abs(command.y) > 65536 || Math.abs(command.rotation) > 1000000) return false;
  if (command.type === 'star') {
    return finite(command.radius) && command.radius > 0 && command.radius <= 65536 && finite(command.innerRadius) && command.innerRadius > 0 && command.innerRadius <= command.radius
      && Number.isInteger(command.points) && command.points >= 3 && command.points <= 8 && typeof command.fill === 'string' && hexColor.test(command.fill);
  }
  if (command.type === 'ellipse') {
    return [command.radiusX, command.radiusY, command.lineWidth].every((value) => finite(value) && value > 0 && value <= 65536) && typeof command.stroke === 'string' && hexColor.test(command.stroke);
  }
  return false;
}

/** Draw commands on an existing Canvas2D context, preserving all caller state. */
export function renderFaceOverlays(ctx, commands) {
  if (!ctx || !Array.isArray(commands)) return 0;
  if (!['save', 'restore', 'beginPath', 'moveTo', 'lineTo', 'closePath', 'fill', 'ellipse', 'stroke'].every((name) => typeof ctx[name] === 'function')) return 0;
  let drawn = 0;
  for (const command of commands.slice(0, 64)) {
    if (!isDrawCommand(command)) continue;
    ctx.save();
    try {
      ctx.globalAlpha *= command.opacity;
      ctx.beginPath();
      if (command.type === 'star') {
        for (let i = 0; i < command.points * 2; i++) {
          const angle = command.rotation - Math.PI / 2 + i * Math.PI / command.points;
          const radius = i % 2 ? command.innerRadius : command.radius;
          const x = command.x + Math.cos(angle) * radius;
          const y = command.y + Math.sin(angle) * radius;
          if (i === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        }
        ctx.closePath();
        ctx.fillStyle = command.fill;
        ctx.fill();
      } else {
        ctx.ellipse(command.x, command.y, command.radiusX, command.radiusY, command.rotation, 0, Math.PI * 2);
        ctx.strokeStyle = command.stroke;
        ctx.lineWidth = command.lineWidth;
        ctx.stroke();
      }
      drawn++;
    } finally {
      ctx.restore();
    }
  }
  return drawn;
}

/**
 * Small temporal controller. It hides and resets immediately when no face is
 * supplied, and resets on timestamp rollback, long gaps or changed command IDs.
 * A new tracked subject must call reset(); this is not a face-identity tracker.
 */
export function createOverlaySmoother({ alpha = 0.45, resetAfterMs = 250 } = {}) {
  if (!finite(alpha) || alpha <= 0 || alpha > 1) throw new RangeError('alpha must be in (0, 1]');
  if (!finite(resetAfterMs) || resetAfterMs <= 0) throw new RangeError('resetAfterMs must be positive');
  let previous = [];
  let lastTime = null;
  const reset = () => { previous = []; lastTime = null; };
  return {
    reset,
    update(commands, timestampMs) {
      if (!finite(timestampMs) || timestampMs < 0) throw new RangeError('timestampMs must be finite and nonnegative');
      if (!Array.isArray(commands) || commands.length === 0 || commands.length > 64 || !commands.every(isDrawCommand)) {
        reset();
        return [];
      }
      const continuity = lastTime !== null && timestampMs >= lastTime && timestampMs - lastTime <= resetAfterMs
        && previous.length === commands.length && previous.every((command, i) => command.id === commands[i].id && command.type === commands[i].type);
      const result = commands.map((command, i) => {
        const next = { ...command };
        if (!continuity) return next;
        for (const key of ['x', 'y', 'radius', 'innerRadius', 'radiusX', 'radiusY', 'lineWidth', 'opacity']) {
          if (finite(command[key]) && finite(previous[i][key])) next[key] = mix(previous[i][key], command[key], alpha);
        }
        const delta = Math.atan2(Math.sin(command.rotation - previous[i].rotation), Math.cos(command.rotation - previous[i].rotation));
        next.rotation = previous[i].rotation + delta * alpha;
        return next;
      });
      previous = result.map((command) => ({ ...command }));
      lastTime = timestampMs;
      return result;
    },
  };
}
