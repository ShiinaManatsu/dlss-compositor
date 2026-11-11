export const DEFAULT_RAM_FILL_PERCENT = 90;
export const DEFAULT_RAM_RESERVE_BYTES = 1024 ** 3;
// Scene-linear half-float path-traced data is nearly incompressible: the user's
// RR multilayer EXRs measured ~45 B/px on disk (70 MB at 935×1663, 2026-08-31),
// i.e. ~0.9 of the raw half-float payload. 0.35 under-planned windows 2.5x.
const EST_ZIP_RATIO = 0.85;
const EXR_HALF_BYTES = 2;
const BEAUTY_CHANNELS = 4;
const GUIDES_CHANNELS = 5;
const SR_CHANNELS = 9;

/** Upper estimate for inject.py's combined multilayer EXR. ZIP can fall back
 * to raw blocks, so budget float32 samples and part/scanline overhead. */
export function estimateRollingFrameBytes(width: number, height: number, mode: string): number {
  const channels = mode === 'rr' ? 27 : SR_CHANNELS;
  const parts = mode === 'rr' ? 10 : 3;
  return Math.ceil((width * height * channels * 4 + parts * (65536 + height * 16)) / 65536) * 65536;
}

export function planRollingRender(
  frameCount: number,
  requestedWorkers: number,
  bytesPerFrame: number,
  volumeBytes: number,
  freeBytes: number,
  maxWindowFrames?: number,
) {
  if (!Number.isSafeInteger(frameCount) || frameCount < 1) throw new Error('渲染帧范围无效');
  const headroomBytes = Math.max(DEFAULT_RAM_RESERVE_BYTES, 2 * bytesPerFrame);
  const budgetBytes = Math.max(0, Math.min(
    Math.floor(volumeBytes * DEFAULT_RAM_FILL_PERCENT / 100), volumeBytes - headroomBytes,
    freeBytes - headroomBytes,
  ));
  const slots = Math.floor(budgetBytes / bytesPerFrame);
  if (slots < 1) throw new Error('缓存空间不足：至少需要容纳一帧未压缩 EXR 和预留空间');
  const workers = Math.min(requestedWorkers, frameCount, slots);
  const framesPerWindow = Math.min(Math.floor(slots / workers), Math.floor(frameCount / workers), maxWindowFrames ?? Infinity);
  return {workers, framesPerWindow, slots, budgetBytes, headroomBytes, bytesPerFrame};
}

export function ramWindowBudget(
  totalBytes: number,
  fillPercent = DEFAULT_RAM_FILL_PERCENT,
  reserveBytes = DEFAULT_RAM_RESERVE_BYTES
): number {
  const total = Math.max(0, Math.trunc(totalBytes));
  if (total <= 0) return 0;
  const pct = Math.min(100, Math.max(1, Math.trunc(fillPercent)));
  const byFill = Math.floor((total * pct) / 100);
  const byReserve = total - Math.max(0, Math.trunc(reserveBytes));
  if (byReserve <= 0) return Math.max(1, byFill);
  return Math.max(1, Math.min(byFill, byReserve));
}

export function estimateExrBytes(width: number, height: number, channels: number): number {
  const w = Math.max(1, Math.trunc(width));
  const h = Math.max(1, Math.trunc(height));
  const nch = Math.max(1, Math.trunc(channels));
  return Math.max(1, Math.floor(w * h * EXR_HALF_BYTES * nch * EST_ZIP_RATIO));
}

export function estimateWindowFrameBytes(
  renderW: number,
  renderH: number,
  outputW: number,
  outputH: number,
  dualPass: boolean
): number {
  if (dualPass) {
    return estimateExrBytes(renderW, renderH, BEAUTY_CHANNELS) + estimateExrBytes(outputW, outputH, GUIDES_CHANNELS);
  }
  return estimateExrBytes(renderW, renderH, SR_CHANNELS);
}

export function planRamWindows(
  frameStart: number,
  frameEnd: number,
  bytesPerFrame: number,
  budgetBytes: number,
  maxFrames?: number | null
): Array<[number, number]> {
  const start = Math.trunc(frameStart);
  const end = Math.trunc(frameEnd);
  if (end < start) return [];
  const per = Math.max(1, Math.trunc(bytesPerFrame));
  const budget = Math.max(1, Math.trunc(budgetBytes));
  let fit = Math.max(1, Math.floor(budget / per));
  if (maxFrames != null && Number.isFinite(maxFrames)) {
    fit = Math.max(1, Math.min(fit, Math.trunc(maxFrames)));
  }
  const windows: Array<[number, number]> = [];
  let cursor = start;
  while (cursor <= end) {
    const last = Math.min(end, cursor + fit - 1);
    windows.push([cursor, last]);
    cursor = last + 1;
  }
  return windows;
}

export function phaseLabel(phase: string, start: number, end: number): string {
  const names: Record<string, string> = {
    beauty: '渲染',
    guides: '引导',
    rebuild: '重建',
    combined: '渲染'
  };
  const title = names[phase] ?? phase;
  const pad = (n: number) => String(Math.trunc(n)).padStart(4, '0');
  return `${title} ${pad(start)}–${pad(end)}`;
}
