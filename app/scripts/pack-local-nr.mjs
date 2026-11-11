import {copyFileSync, existsSync, mkdtempSync, readFileSync, renameSync, rmdirSync, rmSync, writeFileSync} from 'node:fs';
import {execFileSync} from 'node:child_process';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const appDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const dllName = 'nvngx_dlssnr.dll';
const dll = path.join(appDir, '..', 'resources', dllName);
if (!existsSync(dll)) {
  console.log('pack:local-nr: 本机没有 NR DLL，跳过本机安装包');
  process.exit(0);
}

const release = path.resolve(appDir, process.argv[2] ?? 'release');
const {version} = JSON.parse(readFileSync(path.join(appDir, 'package.json'), 'utf8'));
const setupName = `DLSS-Compositor-Setup-${version}.exe`;
const setup = path.join(release, setupName);
if (!existsSync(setup)) throw new Error(`pack:local-nr: 找不到 ${setup}`);

// Keep the dev-only DLL out of the public installer and portable ZIP. NSIS
// already copies the companion DLL into the installed CLI directory.
const bundle = path.join(release, `DLSS-Compositor-Setup-${version}-Local-NR.zip`);
const stage = mkdtempSync(path.join(release, '.local-nr-'));
const readme = 'README-Local-NR.txt';
const zip = path.join(stage, 'bundle.zip');
try {
  copyFileSync(setup, path.join(stage, setupName));
  copyFileSync(dll, path.join(stage, dllName));
  writeFileSync(path.join(stage, readme),
    `本机 NR 安装包\r\n\r\n完整解压后运行 ${setupName}。\r\n安装器会把同目录的 ${dllName} 拷到安装目录。\r\n`);
  execFileSync('tar.exe', ['-a', '-c', '-f', zip, '-C', stage, setupName, dllName, readme], {stdio: 'inherit'});
  renameSync(zip, bundle);
  console.log(`pack:local-nr: ${bundle}`);
} finally {
  for (const name of [setupName, dllName, readme, path.basename(zip)]) rmSync(path.join(stage, name), {force: true});
  rmdirSync(stage);
}
