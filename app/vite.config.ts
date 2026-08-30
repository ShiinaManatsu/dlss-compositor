import {defineConfig} from 'vite';
import react from '@vitejs/plugin-react';
import macros from 'unplugin-parcel-macros';
import optimizeLocales from '@react-aria/optimize-locales-plugin';

// S2's stylesheet registers Adobe Clean / Source Code Pro via remote Typekit
// URLs (use.typekit.net). A desktop app must not fetch UI fonts from the
// network (offline = fallback flash, flaky CDN = slow font swap), and the
// fonts' license does not allow bundling. Strip those @font-face rules at
// build time; the stacks fall through to ui-sans-serif/system-ui (Segoe UI +
// YaHei on Windows).
const stripRemoteFonts = () => ({
  name: 'strip-remote-fonts',
  generateBundle(_options: unknown, bundle: Record<string, {type: string; source: string}>) {
    for (const asset of Object.values(bundle)) {
      if (asset.type === 'asset' && typeof asset.source === 'string' && asset.source.includes('use.typekit.net')) {
        asset.source = asset.source.replace(/@font-face\{[^{}]*use\.typekit\.net[^{}]*\}/g, '');
      }
    }
  }
});

export default defineConfig({
  base: './',
  plugins: [
    stripRemoteFonts(),
    macros.vite(),
    react(),
    {
      ...optimizeLocales.vite({locales: ['zh-CN', 'en-US']}),
      enforce: 'pre'
    }
  ],
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: true
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    target: ['es2022'],
    cssMinify: 'lightningcss',
    rollupOptions: {
      output: {
        manualChunks(id) {
          if (/macro-(.*)\.css$/.test(id) || /@react-spectrum\/s2\/.*\.css$/.test(id)) {
            return 's2-styles';
          }
        }
      }
    }
  }
});
