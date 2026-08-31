import {copyFileSync, existsSync} from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

/**
 * Seeds the dev-only NR feature DLL (nvngx_dlssnr.dll, gitignored resources/)
 * into every local CLI bundle it can find — win-unpacked, any installed copy,
 * and next to the Setup exe (the NSIS script build/installer.nsh picks it up
 * from $EXEDIR at install time, so installing a new version re-seeds itself
 * instead of wiping the previously seeded DLL).
 * Packaging deliberately excludes it (never ship to other machines),
 * so this runs after electron-builder to restore it on THIS machine.
 * No-op when the source DLL is absent (clean checkouts).
 */

const here = path.dirname(fileURLToPath(import.meta.url));
const appDir = path.resolve(here, '..');
const repoRoot = path.resolve(appDir, '..');

const src = path.join(repoRoot, 'resources', 'nvngx_dlssnr.dll');
if (!existsSync(src)) {
  console.log('seed-nr: resources/nvngx_dlssnr.dll 不存在（非开发机），跳过');
  process.exit(0);
}

const targets = [
  path.join(appDir, 'release', 'win-unpacked', 'resources', 'backend', 'cli'),
  path.join(process.env.LOCALAPPDATA ?? '', 'Programs', 'DLSS Compositor', 'resources', 'backend', 'cli'),
];

let seeded = 0;
for (const dir of targets) {
  if (!existsSync(path.join(dir, 'dlss-compositor-cli.exe'))) continue;
  copyFileSync(src, path.join(dir, 'nvngx_dlssnr.dll'));
  console.log(`seed-nr: ${dir}`);
  seeded++;
}
copyFileSync(src, path.join(appDir, 'release', 'nvngx_dlssnr.dll'));
console.log(`seed-nr: ${path.join(appDir, 'release', 'nvngx_dlssnr.dll')}（供安装器 $EXEDIR 拾取）`);
if (seeded === 0) console.log('seed-nr: 没找到可播种的 CLI 目录（先跑 dist）');
