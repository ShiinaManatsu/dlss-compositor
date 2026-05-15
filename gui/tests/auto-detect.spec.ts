/**
 * Coverage for the camera.json auto-detect flow in InputOutputPanel.
 *
 * Approach: mock the dialog:openDirectory IPC handler to return our fixture
 * directory. The renderer then calls cameraAutoDetect + countExrFiles via the
 * real preload bridge, exercising the full chain end-to-end.
 *
 * Fixtures live under gui/tests/fixtures/{with,without}_camera_json/.
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

const mainEntry = path.resolve(__dirname, '..', 'dist-electron', 'main.js');
const FIXTURE_WITH = path.resolve(__dirname, 'fixtures', 'with_camera_json');
const FIXTURE_WITHOUT = path.resolve(__dirname, 'fixtures', 'without_camera_json');

let app: ElectronApplication;
let page: Page;
const userDataDir = path.join(os.tmpdir(), `dlss-pw-autodetect-${Date.now()}`);

test.beforeAll(async () => {
  fs.rmSync(path.resolve(__dirname, '..', 'dlss-compositor.json'), { force: true });
  app = await electron.launch({
    executablePath: require('electron') as unknown as string,
    args: [mainEntry, `--user-data-dir=${userDataDir}`],
  });
  page = await app.firstWindow();
  await page.waitForLoadState('domcontentloaded');
  await page.getByTestId('input-dir-browse').waitFor({ state: 'visible', timeout: 15_000 });
});

test.afterAll(async () => {
  await app?.close();
  fs.rmSync(userDataDir, { recursive: true, force: true });
});

async function mockNextDialog(returnPath: string) {
  await app.evaluate(({ ipcMain }, p) => {
    ipcMain.removeHandler('dialog:openDirectory');
    ipcMain.removeHandler('dialog:openFile');
    ipcMain.handleOnce('dialog:openDirectory', () => p);
    ipcMain.handleOnce('dialog:openFile', () => p);
  }, returnPath);
}

test('camera-data-picker is present in the basic settings panel', async () => {
  const picker = page.getByTestId('camera-data-picker');
  await expect(picker).toBeVisible();
});

test('input-dir-browse triggers EXR count + camera auto-detect (with_camera_json)', async () => {
  await mockNextDialog(FIXTURE_WITH);
  await page.getByTestId('input-dir-browse').click();
  await expect(page.getByTestId('input-dir-field')).toHaveValue(FIXTURE_WITH, { timeout: 10_000 });

  // The fixture has 3 .exr files
  const exrHint = page.getByTestId('exr-count-hint');
  await expect(exrHint).toBeVisible({ timeout: 10_000 });
  await expect(exrHint).toContainText(/Found 3 EXR files/);

  // camera.json was found → auto-detected hint should appear
  const cameraHint = page.getByTestId('camera-auto-detected-hint');
  await expect(cameraHint).toBeVisible({ timeout: 10_000 });
  await expect(cameraHint).toContainText(/Auto-detected/);

  // T6: source resolution is now read from EXR header (not camera.json).
  // The fixture EXRs are 7-byte stubs, so header decoding will fail and the
  // hint surfaces the failure mode rather than a width/height.
  const sourceHint = page.getByTestId('source-resolution-hint');
  await expect(sourceHint).toBeVisible({ timeout: 10_000 });
  await expect(sourceHint).toContainText(/Could not detect|No EXR files|Source:/);
});

test('selecting without_camera_json shows EXR count but no auto-detect hint', async () => {
  await mockNextDialog(FIXTURE_WITHOUT);
  await page.getByTestId('input-dir-browse').click();
  await expect(page.getByTestId('input-dir-field')).toHaveValue(FIXTURE_WITHOUT, { timeout: 10_000 });

  // 2 EXR files
  const exrHint = page.getByTestId('exr-count-hint');
  await expect(exrHint).toBeVisible({ timeout: 10_000 });
  await expect(exrHint).toContainText(/Found 2 EXR files/);

  // No camera.json → no auto-detected hint
  await expect(page.getByTestId('camera-auto-detected-hint')).toHaveCount(0);
});
