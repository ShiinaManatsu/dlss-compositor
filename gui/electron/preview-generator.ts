import { spawn } from 'child_process';
import * as path from 'path';
import * as os from 'os';
import * as fs from 'fs';

// Cache: exrPath → tmpPng path (to avoid re-converting same frame)
const previewCache = new Map<string, string>();

export async function exrToPng(exrPath: string, maxDim: number = 480): Promise<string | null> {
  if (!exrPath) return null;
  
  // Return cached PNG if already converted
  const cached = previewCache.get(exrPath);
  if (cached && fs.existsSync(cached)) return cached;
  
  const basename = path.basename(exrPath, path.extname(exrPath));
  const tmpPng = path.join(os.tmpdir(), `dlss-preview-${basename}-${Date.now()}.png`);
  
  return new Promise((resolve) => {
    const args = [
      '-y',
      '-i', exrPath,
      '-vf', `scale=${maxDim}:-1:flags=lanczos`,
      '-frames:v', '1',
      tmpPng
    ];
    
    const proc = spawn('ffmpeg', args, { stdio: 'pipe' });
    
    proc.once('close', (code) => {
      if (code === 0 && fs.existsSync(tmpPng)) {
        previewCache.set(exrPath, tmpPng);
        // Keep cache small (max 10 entries)
        if (previewCache.size > 10) {
          const firstKey = previewCache.keys().next().value;
          const oldPng = previewCache.get(firstKey);
          previewCache.delete(firstKey);
          try { if (oldPng) fs.unlinkSync(oldPng); } catch {}
        }
        resolve(tmpPng);
      } else {
        resolve(null);
      }
    });
    
    proc.once('error', () => resolve(null));
  });
}

export function cleanupPreviewCache(): void {
  for (const [, pngPath] of previewCache) {
    try { fs.unlinkSync(pngPath); } catch {}
  }
  previewCache.clear();
}
