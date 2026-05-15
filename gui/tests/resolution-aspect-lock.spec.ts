/**
 * T6 ResolutionPanel aspect-lock contract:
 *   - When source is unknown the panel shows only a fallback scale input.
 *   - The legacy free-form W/H inputs (custom-target-width, custom-target-height)
 *     no longer exist anywhere in the DOM — the panel enforces a single primary
 *     dimension + computed counterpart instead.
 *   - The aspect-locked controls (preset select, primary-dim-input,
 *     primary-dim-toggle, computed-target-*) are gated by sourceKnown and
 *     therefore absent on clean launch.
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
const userDataDir = path.join(os.tmpdir(), `dlss-pw-aspectlock-${Date.now()}`);

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

test('source-unknown mode shows the fallback scale input', async () => {
  await expect(page.getByTestId('source-unknown-warning')).toBeVisible();
  const fallback = page.getByTestId('resolution-scale-input-fallback');
  await expect(fallback).toBeVisible();
  await expect(fallback).toHaveAttribute('type', 'number');
});

test('source-unknown mode hides the aspect-locked primary-dim input', async () => {
  await expect(page.getByTestId('primary-dim-input')).toHaveCount(0);
  await expect(page.getByTestId('primary-dim-toggle')).toHaveCount(0);
});

test('source-unknown mode hides the aspect-locked scale input', async () => {
  // The aspect-locked branch uses resolution-scale-input; only the fallback
  // testid should exist while source is unknown.
  await expect(page.getByTestId('resolution-scale-input')).toHaveCount(0);
});

test('source-unknown mode hides the orientation-aware preset select', async () => {
  await expect(page.getByTestId('resolution-preset-select')).toHaveCount(0);
});

test('legacy free-form custom-target W/H inputs are removed', async () => {
  await expect(page.getByTestId('custom-target-width')).toHaveCount(0);
  await expect(page.getByTestId('custom-target-height')).toHaveCount(0);
});

test('fallback scale input round-trips a value through the store', async () => {
  const fallback = page.getByTestId('resolution-scale-input-fallback');
  // SRSettings's scale-input mirrors state.scaleFactor — verify they stay in sync.
  await fallback.fill('3');
  await fallback.press('Tab');
  await expect(page.getByTestId('scale-input')).toHaveValue('3');
  // Reset
  await fallback.fill('2');
  await fallback.press('Tab');
});
