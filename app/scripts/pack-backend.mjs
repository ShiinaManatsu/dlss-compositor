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

// --- VC++ runtime: the CLI is /MD, Electron is not — a clean Windows box has
// no msvcp140/vcruntime140, so ship the redist DLLs app-local next to the exe
// (Microsoft-supported deployment; app dir wins over System32 at load time).
const crtDlls = [
  'msvcp140.dll',
  'msvcp140_1.dll',
  'msvcp140_2.dll',
  'msvcp140_atomic_wait.dll',
  'msvcp140_codecvt_ids.dll',
  'vcruntime140.dll',
  'vcruntime140_1.dll',
];
const vsRoot = 'C:/Program Files/Microsoft Visual Studio';
let crtDir = null;
try {
  const findRedist = (redistRoot) => {
    if (!existsSync(redistRoot)) return null;
    const versions = readdirSync(redistRoot)
      .filter((v) => /^\d+\.\d+\.\d+$/.test(v))
      .sort((a, b) => a.localeCompare(b))
      .reverse();
    for (const v of versions) {
      const candidate = path.join(redistRoot, v, 'x64', 'Microsoft.VC143.CRT');
      if (existsSync(path.join(candidate, 'msvcp140.dll'))) return candidate;
    }
    return null;
  };
  for (const entry of readdirSync(vsRoot)) {
    const base = path.join(vsRoot, entry);
    // VS2019/2022 layout: <edition>/VC/Redist/MSVC — VS2026 nests once more:
    // <year>/<edition>/VC/Redist/MSVC.
    crtDir = findRedist(path.join(base, 'VC', 'Redist', 'MSVC')) ??
      (() => {
        if (!statSync(base).isDirectory()) return null;
        for (const sub of readdirSync(base)) {
          const found = findRedist(path.join(base, sub, 'VC', 'Redist', 'MSVC'));
          if (found) return found;
        }
        return null;
      })();
    if (crtDir) break;
  }
} catch {
  /* fall through to the loud failure below */
}
if (!crtDir) {
  fail('找不到 VC++ 运行库（VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT）——装好 VS 或手动指定');
}
for (const dll of crtDlls) {
  copyTo(path.join(crtDir, dll), cliDest);
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
