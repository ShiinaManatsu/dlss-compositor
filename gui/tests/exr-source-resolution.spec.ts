/**
 * T6 source-resolution detection contract:
 *   - Source resolution is read from the first EXR's header (NOT camera.json).
 *   - On clean launch (no input dir) there is no source-resolution-hint and
 *     no auto-detected camera hint.
 *   - InputOutputPanel renders the input field and Browse button regardless.
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
const userDataDir = path.join(os.tmpdir(), `dlss-pw-exrsrcres-${Date.now()}`);

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

test('input-dir-field renders empty on clean launch', async () => {
  const inputField = page.getByTestId('input-dir-field');
  await expect(inputField).toBeVisible();
  await expect(inputField).toHaveValue('');
});

test('no source-resolution-hint shown before any directory is picked', async () => {
  // The hint testid only renders after handleInputBrowse populates
  // sourceDetectionInfo. On clean launch there should be zero hints.
  await expect(page.getByTestId('source-resolution-hint')).toHaveCount(0);
});

test('no camera-auto-detected-hint shown before any directory is picked', async () => {
  await expect(page.getByTestId('camera-auto-detected-hint')).toHaveCount(0);
});

test('source-unknown-warning is visible while no source resolution is known', async () => {
  // ResolutionPanel's source-unknown branch is the canonical "source unknown"
  // signal — it doesn't depend on InputOutputPanel having run yet.
  await expect(page.getByTestId('source-unknown-warning')).toBeVisible();
});

test('input-dir browse button is enabled on clean launch', async () => {
  await expect(page.getByTestId('input-dir-browse')).toBeEnabled();
});
