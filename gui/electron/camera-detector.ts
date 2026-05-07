import * as fs from 'fs';
import * as path from 'path';

export function findCameraJson(inputDir: string): string | null {
  try {
    const candidate = path.join(inputDir, 'camera.json');
    return fs.existsSync(candidate) ? candidate : null;
  } catch {
    return null;
  }
}

export interface SourceResolution {
  width: number;
  height: number;
}

export function readSourceResolution(cameraJsonPath: string): SourceResolution | null {
  try {
    const raw = fs.readFileSync(cameraJsonPath, 'utf-8');
    const data = JSON.parse(raw);
    const w = data.render_width;
    const h = data.render_height;
    if (typeof w === 'number' && typeof h === 'number' && w > 0 && h > 0) {
      return { width: w, height: h };
    }
    return null;
  } catch {
    return null;
  }
}