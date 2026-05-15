import { spawn, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import path from 'node:path';
import os from 'node:os';
import fs from 'node:fs';

const execFileAsync = promisify(execFile);

export type PreviewFailureReason =
  | 'ffmpeg-not-found'
  | 'exr-decode-error'
  | 'timeout'
  | 'unknown';

export type PreviewResult =
  | { success: true; pngPath: string }
  | { success: false; reason: PreviewFailureReason; detail: string };

// Cache: exrPath → PreviewResult (to avoid re-converting the same frame)
const previewCache = new Map<string, PreviewResult>();

// FFmpeg availability — probed once at startup.
//   undefined → not yet probed
//   string    → resolved executable to invoke (currently always 'ffmpeg' from PATH)
//   null      → probed and not available
let ffmpegPath: string | null | undefined = undefined;

export async function locateFFmpeg(): Promise<string | null> {
  if (ffmpegPath !== undefined) return ffmpegPath;

  try {
    await execFileAsync('ffmpeg', ['-version'], { timeout: 3000 });
    ffmpegPath = 'ffmpeg';
    return ffmpegPath;
  } catch {
    ffmpegPath = null;
    return null;
  }
}

/** Test-only helper: reset the memoised FFmpeg probe result. */
export function _resetFFmpegProbe(): void {
  ffmpegPath = undefined;
}

export async function generatePreview(
  exrPath: string,
  opts?: { timeoutMs?: number; maxDim?: number }
): Promise<PreviewResult> {
  const timeoutMs = opts?.timeoutMs ?? 10_000;
  const maxDim = opts?.maxDim ?? 480;

  if (!exrPath) {
    return { success: false, reason: 'unknown', detail: 'Empty path' };
  }

  // Return cached result (drop stale entries pointing at deleted files).
  const cached = previewCache.get(exrPath);
  if (cached) {
    if (cached.success && !fs.existsSync(cached.pngPath)) {
      previewCache.delete(exrPath);
    } else {
      return cached;
    }
  }

  const ffmpeg = await locateFFmpeg();
  if (!ffmpeg) {
    return {
      success: false,
      reason: 'ffmpeg-not-found',
      detail: 'FFmpeg not found in PATH',
    };
  }

  const basename = path.basename(exrPath, path.extname(exrPath));
  const tmpPng = path.join(
    os.tmpdir(),
    `dlss-preview-${basename}-${Date.now()}.png`
  );

  return new Promise<PreviewResult>((resolve) => {
    const args = [
      '-y',
      '-i', exrPath,
      '-vf', `scale=${maxDim}:-1:flags=lanczos`,
      '-frames:v', '1',
      tmpPng,
    ];
    const proc = spawn(ffmpeg, args, { stdio: 'pipe' });
    const stderrChunks: Buffer[] = [];

    proc.stderr?.on('data', (chunk: Buffer) => stderrChunks.push(chunk));

    let timedOut = false;
    const timer = setTimeout(() => {
      timedOut = true;
      try { proc.kill('SIGTERM'); } catch { /* already exited */ }
    }, timeoutMs);

    proc.once('close', (code) => {
      clearTimeout(timer);

      if (timedOut) {
        resolve({
          success: false,
          reason: 'timeout',
          detail: `FFmpeg timed out after ${timeoutMs}ms`,
        });
        return;
      }

      if (code === 0 && fs.existsSync(tmpPng)) {
        const result: PreviewResult = { success: true, pngPath: tmpPng };
        previewCache.set(exrPath, result);
        // Keep cache small (max 10 entries).
        if (previewCache.size > 10) {
          const firstKey = previewCache.keys().next().value;
          if (firstKey) {
            const old = previewCache.get(firstKey);
            previewCache.delete(firstKey);
            if (old?.success) {
              try { fs.unlinkSync(old.pngPath); } catch { /* best effort */ }
            }
          }
        }
        resolve(result);
        return;
      }

      const stderr = Buffer.concat(stderrChunks).toString();
      const isDecodeError =
        stderr.includes('Invalid data') ||
        stderr.includes('No such decoder') ||
        stderr.includes('codec not found') ||
        stderr.includes('Error while opening encoder') ||
        stderr.includes('Invalid argument') ||
        stderr.includes('does not contain any stream');

      resolve(
        isDecodeError
          ? {
              success: false,
              reason: 'exr-decode-error',
              detail: stderr.slice(0, 200),
            }
          : {
              success: false,
              reason: 'unknown',
              detail: `FFmpeg exited with code ${code}: ${stderr.slice(0, 200)}`,
            }
      );
    });

    proc.once('error', (err) => {
      clearTimeout(timer);
      const isMissing = err.message.includes('ENOENT');
      resolve(
        isMissing
          ? {
              success: false,
              reason: 'ffmpeg-not-found',
              detail: 'FFmpeg not found in PATH',
            }
          : { success: false, reason: 'unknown', detail: err.message }
      );
    });
  });
}

export function cleanupPreviewCache(): void {
  for (const [, result] of previewCache) {
    if (result.success) {
      try { fs.unlinkSync(result.pngPath); } catch { /* best effort */ }
    }
  }
  previewCache.clear();
}
