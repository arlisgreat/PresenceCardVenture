import type { FaceLandmark } from './live.js';
export interface TrackingResult { trackingAccepted: boolean; landmarks: FaceLandmark[] }
export interface LiveTrackingResult extends TrackingResult { status: 'ok' | 'busy' | 'throttled' }
export interface AssetOptions { wasmRoot: string; modelAssetPath: string }
export interface MediaPipeVisionSdk {
  FilesetResolver: { forVisionTasks(root: string, useModule?: boolean): Promise<unknown> };
  FaceLandmarker: { createFromOptions(files: unknown, options: Record<string, unknown>): Promise<{
    detectForVideo(frame: ImageBitmap, timestampMs: number): { faceLandmarks?: FaceLandmark[][] };
    close(): void;
  }> };
}
/** Worker-side only. The caller closes its borrowed frame. */
export function createMediaPipeDetector(vision: MediaPipeVisionSdk, options: AssetOptions): Promise<{
  detect(frame: ImageBitmap, timestampMs: number): TrackingResult;
  close(): void;
}>;
export interface LiveTrackerOptions extends AssetOptions {
  maxFps?: number;
  initTimeoutMs?: number;
  frameTimeoutMs?: number;
  workerFactory?: (url: URL, options: WorkerOptions) => Worker;
}
/** UI-thread bridge; each detect call consumes the bitmap, even when it drops the frame. */
export function createLiveTracker(options: LiveTrackerOptions): Promise<{
  detect(bitmap: ImageBitmap, timestampMs: number): Promise<LiveTrackingResult>;
  close(): void;
}>;
