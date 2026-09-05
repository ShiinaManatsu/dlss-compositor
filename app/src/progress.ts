export function jobCount(start: number, end: number) {
  return Math.max(1, end - start + 1);
}

/** Blender `frame_current` only counts if it sits on this job. Hinter / scene playhead is dropped. */
export function mapBlenderFrame(frame: number, start: number, end: number): number | null {
  if (!Number.isFinite(frame)) return null;
  const n = Math.trunc(frame);
  if (n >= start && n <= end) return n;
  return null;
}

export function playheadRatio(current: number, start: number, end: number): number {
  if (end <= start) return current >= end ? 1 : 0;
  return Math.min(1, Math.max(0, (current - start) / (end - start)));
}

// 帧时间与剩余估算都用最近 10 帧的平均 — 单帧间隔跳动太大（depsgraph 停顿、
// 轮询粒度都会让某一帧翻倍）。
const ROLLING_WINDOW = 10;

export type FrameTiming = {
  samplesMs: number[];
  lastAdvanceAt: number | null;
  lastMs: number | null;
};

export function emptyFrameTiming(): FrameTiming {
  return {samplesMs: [], lastAdvanceAt: null, lastMs: null};
}

/** Job start: clock begins, but there is no completed-frame sample yet. */
export function startFrameTiming(now: number): FrameTiming {
  return {samplesMs: [], lastAdvanceAt: now, lastMs: null};
}

/**
 * Record a playhead advance. The first observation after start is not a sample
 * (the current frame is still in flight). Later advances split elapsed time
 * across the jumped frame count.
 */
export function recordFrameAdvance(
  timing: FrameTiming,
  prevFrame: number,
  nextFrame: number,
  now: number
): FrameTiming {
  if (!Number.isFinite(nextFrame) || !Number.isFinite(prevFrame)) return timing;
  const advanced = Math.trunc(nextFrame) - Math.trunc(prevFrame);
  if (advanced <= 0) return timing;
  if (timing.lastAdvanceAt == null) {
    return {...timing, lastAdvanceAt: now};
  }
  const elapsed = now - timing.lastAdvanceAt;
  if (elapsed <= 0) return {...timing, lastAdvanceAt: now};
  const perFrame = elapsed / advanced;
  const added = Array.from({length: advanced}, () => perFrame);
  const samplesMs = [...timing.samplesMs, ...added].slice(-ROLLING_WINDOW);
  return {samplesMs, lastAdvanceAt: now, lastMs: perFrame};
}

export function rollingAverageMs(timing: FrameTiming): number | null {
  if (timing.samplesMs.length === 0) return null;
  let sum = 0;
  for (const sample of timing.samplesMs) sum += sample;
  return sum / timing.samplesMs.length;
}

/** Short clock: `2.1s`, `21s`, `1.5m`, `14m`, `1h 3m`. */
export function formatCompactDuration(ms: number): string {
  if (!Number.isFinite(ms) || ms < 0) return '';
  const seconds = ms / 1000;
  if (seconds < 10) return `${seconds.toFixed(1)}s`;
  if (seconds < 60) return `${Math.round(seconds)}s`;
  const minutes = seconds / 60;
  if (minutes < 10) return `${minutes.toFixed(1)}m`;
  if (minutes < 60) return `${Math.round(minutes)}m`;
  const hours = Math.floor(minutes / 60);
  const rest = Math.round(minutes - hours * 60);
  if (rest === 0) return `${hours}h`;
  return `${hours}h ${rest}m`;
}

/**
 * Bottom-bar line after the first frames complete.
 * Both the rate and the remaining estimate use the rolling average of the
 * last frames — the single-frame interval jumps too much to display.
 * Returns null before any sample exists — never a fake ETA.
 */
export function formatEtaLine(
  timing: FrameTiming,
  remainingFrames: number
): string | null {
  if (timing.lastMs == null) return null;
  const avg = rollingAverageMs(timing);
  if (avg == null) return null;
  const remainingMs = avg * Math.max(0, remainingFrames);
  return `${formatCompactDuration(avg)}/帧 · 剩余 ${formatCompactDuration(remainingMs)}`;
}
