/**
 * Coverage for the T6 ResolutionPanel:
 *   - source-unknown mode shows warning + fallback scale input
 *   - preset select / primary-dim-input only render once source resolution
 *     is known (this happens after EXR header detection in InputOutputPanel)
 *   - Old DLAA/4K/1440p/1080p/Custom preset selector is gone — replaced by
 *     orientation-aware "Same as Source / 1080p / 1440p / 4K" presets
 *   - Custom width/height fields are gone — replaced by single primary-dim
 *     input + computed-target display + scale input
 */

import { test, expect, type ElectronApplication, type Page } from '@playwright/test';
import { _electron as electron } from 'playwright';
import path from 'node:path';
import os from 'node:os';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const require = createRequire(import.meta.url);

let app: ElectronApplication;
let page: Page;

const mainEntry = path.resolve(__dirname, '..', 'dist-electron', 'main.js');
const userDataDir = path.join(os.tmpdir(), `dlss-pw-resolution-${Date.now()}`);

test.beforeAll(async () => {
  fs.rmSync(path.resolve(__dirname, '..', 'dlss-compositor.json'), { force: true });
  app = await electron.launch({
    executablePath: require('electron') as unknown as string,
    args: [mainEntry, `--user-data-dir=${userDataDir}`],
  });
  page = await app.firstWindow();
  await page.waitForLoadState('domcontentloaded');
  await page.getByTestId('settings-panel').waitFor({ state: 'visible', timeout: 15_000 });
});

test.afterAll(async () => {
  await app?.close();
  fs.rmSync(userDataDir, { recursive: true, force: true });
});

test('source-unknown mode shows warning on fresh launch (no input dir)', async () => {
  await expect(page.getByTestId('source-unknown-warning')).toBeVisible();
});

test('source-unknown mode shows fallback scale input', async () => {
  const fallback = page.getByTestId('resolution-scale-input-fallback');
  await expect(fallback).toBeVisible();
  await expect(fallback).toHaveAttribute('type', 'number');
});

test('source-unknown mode hides preset select and primary-dim input', async () => {
  // The aspect-locked controls are gated by sourceKnown
  await expect(page.getByTestId('resolution-preset-select')).toHaveCount(0);
  await expect(page.getByTestId('primary-dim-input')).toHaveCount(0);
});

test('legacy custom width/height inputs no longer exist', async () => {
  await expect(page.getByTestId('custom-target-width')).toHaveCount(0);
  await expect(page.getByTestId('custom-target-height')).toHaveCount(0);
});

test('legacy target-preset-select no longer exists', async () => {
  // Old DLAA/4K/1440p/1080p/Custom selector replaced by resolution-preset-select
  await expect(page.getByTestId('target-preset-select')).toHaveCount(0);
});
