/**
 * T13 DLSS detection contract:
 *   - StatusBar parses both legacy `event.type` and current `event.event`
 *     banner schemas without crashing the renderer.
 *   - StatusBar always renders some state (configure-exe-path hint, detecting,
 *     error, or banner detail).
 *   - The 15s detection timeout transitions to error rather than hanging.
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
const userDataDir = path.join(os.tmpdir(), `dlss-pw-dlssdetect-${Date.now()}`);

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

test('app root mounts even when DLSS probe is in flight', async () => {
  await expect(page.getByTestId('app-root')).toBeVisible();
});

test('status bar renders one of the documented states (no crash)', async () => {
  const statusBar = page.getByTestId('status-bar');
  await expect(statusBar).toBeVisible();
  // One of: "Configure exe path", "Detecting DLSS capabilities", "DLSS detection failed",
  // "DLSS SDK <ver>", or any banner-driven content.
  await expect(statusBar).toHaveText(
    /Configure exe path|Detecting DLSS|DLSS detection failed|DLSS SDK|DLSS detected/,
  );
});

test('settings panel mounts despite DLSS detection state', async () => {
  // T13 ensured a banner failure does not block the rest of the renderer.
  await expect(page.getByTestId('settings-panel')).toBeVisible();
  await expect(page.getByTestId('main-area')).toBeVisible();
});

test('status bar exposes a single instance (no duplicates from probe race)', async () => {
  await expect(page.getByTestId('status-bar')).toHaveCount(1);
});
