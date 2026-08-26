import * as esbuild from 'esbuild';

await esbuild.build({
  entryPoints: ['electron/main.ts'],
  outfile: 'out/main.js',
  bundle: true,
  platform: 'node',
  format: 'esm',
  target: 'node20',
  packages: 'external',
  external: ['electron'],
  sourcemap: true,
});

await esbuild.build({
  entryPoints: ['electron/preload.ts'],
  outfile: 'out/preload.cjs',
  bundle: true,
  platform: 'node',
  format: 'cjs',
  target: 'node20',
  external: ['electron'],
  sourcemap: true,
});
