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
