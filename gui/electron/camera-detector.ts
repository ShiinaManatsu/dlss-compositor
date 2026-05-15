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
