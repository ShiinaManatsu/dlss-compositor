import {spawn} from 'node:child_process';
import net from 'node:net';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import {build} from 'esbuild';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));

await build({
  entryPoints: [path.join(root, 'electron/main.ts')],
  outfile: path.join(root, 'out/main.js'),
  bundle: true,
  platform: 'node',
  format: 'esm',
  target: 'node20',
  packages: 'external',
  external: ['electron'],
  sourcemap: true,
});

await build({
  entryPoints: [path.join(root, 'electron/preload.ts')],
  outfile: path.join(root, 'out/preload.cjs'),
  bundle: true,
  platform: 'node',
  format: 'cjs',
  target: 'node20',
  external: ['electron'],
  sourcemap: true,
});

const vite = spawn('npx', ['vite', '--port', '5173', '--strictPort'], {
  cwd: root,
  stdio: 'inherit',
  shell: true,
});

async function waitUntilListening(port, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const ok = await new Promise((resolve) => {
      const sock = net.connect(port, '127.0.0.1', () => {
        sock.end();
        resolve(true);
      });
      sock.on('error', () => resolve(false));
    });
    if (ok) return;
    await new Promise((r) => setTimeout(r, 200));
  }
  throw new Error('vite port did not open');
}

await waitUntilListening(5173, 20000);

const electron = spawn('npx', ['electron', '.'], {
  cwd: root,
  stdio: 'inherit',
  shell: true,
  env: {
    ...process.env,
    ELECTRON_RENDERER_URL: 'http://127.0.0.1:5173',
  },
});

function shutdown() {
  electron.kill();
  vite.kill();
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);

electron.on('exit', (code) => {
  vite.kill();
  process.exit(code ?? 0);
});
