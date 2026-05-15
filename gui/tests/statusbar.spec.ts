/**
 * Coverage for the StatusBar component which displays DLSS SDK availability.
 * With no exePath configured (fresh launch), it shows the "configure exe path" hint.
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
const userDataDir = path.join(os.tmpdir(), `dlss-pw-statusbar-${Date.now()}`);

test.beforeAll(async () => {
  fs.rmSync(path.resolve(__dirname, '..', 'dlss-compositor.json'), { force: true });
  app = await electron.launch({
    executablePath: require('electron') as unknown as string,
    args: [mainEntry, `--user-data-dir=${userDataDir}`],
  });
  page = await app.firstWindow();
  await page.waitForLoadState('domcontentloaded');
  await page.getByTestId('app-root').waitFor({ state: 'visible', timeout: 15_000 });
});

test.afterAll(async () => {
  await app?.close();
  fs.rmSync(userDataDir, { recursive: true, force: true });
});

test('status bar element is present', async () => {
  await expect(page.getByTestId('status-bar')).toBeVisible();
});

test('status bar shows configure-exe-path hint on fresh launch', async () => {
  // Fresh launch has no exePath → status bar shows guidance
  const statusBar = page.getByTestId('status-bar');
  await expect(statusBar).toContainText(/Configure exe path/i);
});

test('status bar sits above the settings panel and main area', async () => {
  // Geometry sanity: status bar is above the layout body
  const statusBox = await page.getByTestId('status-bar').boundingBox();
  const settingsBox = await page.getByTestId('settings-panel').boundingBox();
  expect(statusBox).not.toBeNull();
  expect(settingsBox).not.toBeNull();
  expect(statusBox!.y + statusBox!.height).toBeLessThanOrEqual(settingsBox!.y + 1);
});
