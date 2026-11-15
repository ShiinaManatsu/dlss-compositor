import {existsSync} from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {Arch, build, Platform} from 'electron-builder';

const appDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const repo = path.dirname(appDir);
const dllName = 'nvngx_dlssnr.dll';
const dll = path.join(repo, 'resources', dllName);
if (!existsSync(dll)) throw new Error(`dist:local-nr: 找不到本机 NR DLL：${dll}`);

process.env.CSC_IDENTITY_AUTO_DISCOVERY ??= 'false';
await build({
  projectDir: appDir,
  targets: Platform.WINDOWS.createTarget('nsis', Arch.x64),
  publish: 'never',
  config: {
    electronDist: path.join(appDir, 'node_modules', 'electron', 'dist'),
    // electron-builder merges this array with package.json. Supply only the
    // extra DLL so the backend files aren't copied twice in parallel.
    extraResources: [{from: dll, to: `backend/cli/${dllName}`}],
    nsis: {include: null, differentialPackage: false, artifactName: 'DLSS-Compositor-Setup-${version}.${ext}'},
  },
});
