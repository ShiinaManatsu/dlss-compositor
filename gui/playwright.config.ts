import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: './tests',
  timeout: 30_000,
  expect: { timeout: 10_000 },
  fullyParallel: false,   // Electron tests must run serially
  workers: 1,
  reporter: 'list',
  use: {
    screenshot: 'only-on-failure',
    trace: 'off',
  },
});
