import {defineConfig} from 'vite';
import react from '@vitejs/plugin-react';
import macros from 'unplugin-parcel-macros';
import optimizeLocales from '@react-aria/optimize-locales-plugin';

export default defineConfig({
  base: './',
  plugins: [
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
