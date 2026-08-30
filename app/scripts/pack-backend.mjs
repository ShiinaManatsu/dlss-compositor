import {copyFileSync, existsSync, mkdirSync, readdirSync, rmSync, statSync} from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

/**
 * Assembles app/pack/backend — the runtime bundle electron-builder ships as
 * resources/backend:
 *   cli/         dlss-compositor-cli.exe + *.spv + locked 310.7.129.0 nvngx DLLs
 *   pipeline/    Blender-side python (runs inside Blender, not system python)
 *   blender-plugin/dlss_compositor/lut_generator.py   (imported by inject.py)
 *
 * NGX core (`nvngx.dll`) comes from the driver and is never bundled.
 */

const here = path.dirname(fileURLToPath(import.meta.url));
const appDir = path.resolve(here, '..');
const repoRoot = path.resolve(appDir, '..');

const cliDir = [process.env.DC_CLI_DIR, path.join(repoRoot, 'build', 'x64-windows-msvc-release', 'Release')]
  .filter(Boolean)
  .find((dir) => existsSync(path.join(dir, 'dlss-compositor-cli.exe')));

if (!cliDir) {
  console.error('pack-backend: 找不到 dlss-compositor-cli.exe（先跑 tools/_rebuild_cli.bat，或设 DC_CLI_DIR）');
  process.exit(1);
}

const dest = path.join(appDir, 'pack', 'backend');
rmSync(dest, {recursive: true, force: true});

function fail(message) {
  console.error(`pack-backend: ${message}`);
  process.exit(1);
}

function copyTo(from, toDir) {
  if (!existsSync(from)) fail(`缺少 ${from}`);
  mkdirSync(toDir, {recursive: true});
  copyFileSync(from, path.join(toDir, path.basename(from)));
}

// --- cli bundle: exe + shaders from the build dir, DLLs from resources/ ---
const cliDest = path.join(dest, 'cli');
mkdirSync(cliDest, {recursive: true});
copyFileSync(path.join(cliDir, 'dlss-compositor-cli.exe'), path.join(cliDest, 'dlss-compositor-cli.exe'));
for (const name of readdirSync(cliDir)) {
  if (name.endsWith('.spv')) {
    copyFileSync(path.join(cliDir, name), path.join(cliDest, name));
  }
}
for (const dll of ['nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssg.dll']) {
  copyTo(path.join(repoRoot, 'resources', dll), cliDest);
}

// --- pipeline python (imported by inject.py) ---
const pipelineDest = path.join(dest, 'pipeline');
for (const script of [
  'inject.py',
  'inspect_blend.py',
  'inspect_scene.py',
  'jitter.py',
  'session_config.py',
  'ramdisk.py',
]) {
  copyTo(path.join(repoRoot, 'pipeline', script), pipelineDest);
}

// --- lut generator from the blender plugin (inject.py sys.path) ---
copyTo(
  path.join(repoRoot, 'blender-plugin', 'dlss_compositor', 'lut_generator.py'),
  path.join(dest, 'blender-plugin', 'dlss_compositor')
);

function dirSize(dir) {
  let total = 0;
  for (const entry of readdirSync(dir, {withFileTypes: true})) {
    const full = path.join(dir, entry.name);
    total += entry.isDirectory() ? dirSize(full) : statSync(full).size;
  }
  return total;
}

console.log(
  `pack-backend: ${dest} ${(dirSize(dest) / 1048576).toFixed(1)} MB (cli from ${cliDir})`
);
