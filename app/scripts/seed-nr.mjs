import {copyFileSync, existsSync, readFileSync} from 'node:fs';
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
const {build} = JSON.parse(readFileSync(path.join(appDir, 'package.json'), 'utf8'));
const releaseDir = path.resolve(appDir, build.directories.output);

const src = path.join(repoRoot, 'resources', 'nvngx_dlssnr.dll');
if (!existsSync(src)) {
  console.log('seed-nr: resources/nvngx_dlssnr.dll 不存在（非开发机），跳过');
  process.exit(0);
}

const targets = [
  path.join(releaseDir, 'win-unpacked', 'resources', 'backend', 'cli'),
  path.join(process.env.LOCALAPPDATA ?? '', 'Programs', 'DLSS Compositor', 'resources', 'backend', 'cli'),
];

let seeded = 0;
for (const dir of targets) {
  if (!existsSync(path.join(dir, 'dlss-compositor-cli.exe'))) continue;
  try {
    copyFileSync(src, path.join(dir, 'nvngx_dlssnr.dll'));
    console.log(`seed-nr: ${dir}`);
    seeded++;
  } catch (err) {
    // A running app with an open NR session keeps the DLL loaded/locked —
    // warn and continue, seeding can happen after the app closes.
    console.warn(`seed-nr: FAILED ${dir}: ${err?.message ?? err}`);
  }
}
try {
  const sidecar = path.join(releaseDir, 'nvngx_dlssnr.dll');
  copyFileSync(src, sidecar);
  console.log(`seed-nr: ${sidecar}（供安装器 $EXEDIR 拾取）`);
} catch (err) {
  console.warn(`seed-nr: FAILED release copy: ${err?.message ?? err}`);
}
if (seeded === 0) console.log('seed-nr: 没找到可播种的 CLI 目录（先跑 dist）');
