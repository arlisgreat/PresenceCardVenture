export const FACE_OVERLAY_VERSION: string;
export const FACE_ANCHORS: Readonly<{ eyeA: 33; eyeB: 263; nose: 1; sideA: 234; sideB: 454 }>;
export interface FaceLandmark { x: number; y: number; z?: number }
export interface OverlayOptions {
  width: number;
  height: number;
  style?: 'cheek-stars' | 'orbit';
  /** Coordinates must come from the unmirrored input; mirror output only once. */
  mirrored?: boolean;
  /** Must be true for an accepted detector result; false/absence hides effects. */
  trackingAccepted?: boolean;
  /** Optional measured confidence, never synthesized for a detector lacking it. */
  confidence?: number;
  minConfidence?: number;
}
interface OverlayBase { id: string; x: number; y: number; rotation: number; opacity: number }
export interface StarOverlay extends OverlayBase { type: 'star'; radius: number; innerRadius: number; points: number; fill: string }
export interface EllipseOverlay extends OverlayBase { type: 'ellipse'; radiusX: number; radiusY: number; lineWidth: number; stroke: string }
export type FaceOverlay = StarOverlay | EllipseOverlay;
export function buildFaceOverlays(landmarks: FaceLandmark[], options: OverlayOptions): FaceOverlay[];
/** Number of valid commands drawn. Does not clear the canvas or acquire a camera. */
export function renderFaceOverlays(ctx: CanvasRenderingContext2D, commands: FaceOverlay[]): number;
export function createOverlaySmoother(options?: { alpha?: number; resetAfterMs?: number }): {
  update(commands: FaceOverlay[], timestampMs: number): FaceOverlay[];
  reset(): void;
};
