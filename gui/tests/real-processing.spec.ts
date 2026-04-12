/**
 * Real end-to-end processing tests using the Electron GUI.
 *
 * Test 2: Float scale 1.5× (MaxQuality)
 * Test 3: Standard 2× upscale
 * Test 4: Combined RR+FG 2× (scale 2 + interpolate 2x + camera.json)
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

const EXE_PATH    = 'F:\\Projects\\GitRepos\\dlss-compositor\\build\\Release\\dlss-compositor.exe';
const INPUT_DIR   = 'E:\\Render Output\\Compositing\\SittingSex1\\sequences\\dlss_test';
const CAMERA_JSON = 'E:\\Render Output\\Compositing\\SittingSex1\\sequences\\dlss_test\\camera.json';
const OUT_1_5x    = 'E:\\Render Output\\Compositing\\SittingSex1\\sequences\\out_upscale_1.5x';
const OUT_2x      = 'E:\\Render Output\\Compositing\\SittingSex1\\sequences\\out_upscale_2x';
const OUT_RRFG    = 'E:\\Render Output\\Compositing\\SittingSex1\\sequences\\out_rrfg_2x';

const EVIDENCE    = 'F:\\Projects\\GitRepos\\dlss-compositor\\.sisyphus\\evidence';
const mainEntry   = path.resolve(__dirname, '..', 'dist-electron', 'main.js');

// Pre-seed exe path in electron-store so the app launches ready.
// electron-store writes to {userData}/{store-name}.json (flat, no subdirectory).
// The store name in main.js is "dlss-compositor-settings", so the file is
// {userData}/dlss-compositor-settings.json with the same flat structure.
async function launchApp(userDataDir: string): Promise<{ app: ElectronApplication; page: Page }> {
  fs.mkdirSync(userDataDir, { recursive: true });
  fs.writeFileSync(
    path.join(userDataDir, 'dlss-compositor-settings.json'),
    JSON.stringify({
      exePath: EXE_PATH,
      lastInputDir: '',
      lastOutputDir: '',
      scaleFactor: 2,
      quality: 'MaxQuality',
      interpolateFactor: 0,
      exrCompression: 'dwaa',
      exrDwaQuality: 95,
      memoryBudgetGB: 8,
      tonemapMode: 'pq',
      inverseTonemapEnabled: true,
    }),
  );

  const app = await electron.launch({
    executablePath: require('electron') as unknown as string,
    args: [mainEntry, `--user-data-dir=${userDataDir}`],
  });
  const page = await app.firstWindow();
  await page.waitForLoadState('domcontentloaded');
  await page.getByTestId('exe-path-field').waitFor({ state: 'visible', timeout: 15_000 });
  return { app, page };
}

// All path fields are readOnly — they're filled via Browse buttons + native dialogs.
// We intercept the IPC handler on the main process side so Browse returns our path directly.
async function mockNextDialog(app: ElectronApplication, returnPath: string) {
  await app.evaluate(({ ipcMain }, path) => {
    // Remove existing handler, replace with one-shot mock
    ipcMain.removeHandler('dialog:openDirectory');
    ipcMain.removeHandler('dialog:openFile');
    ipcMain.handleOnce('dialog:openDirectory', () => path);
    ipcMain.handleOnce('dialog:openFile', () => path);
  }, returnPath);
}

async function configureIO(app: ElectronApplication, page: Page, inputDir: string, outputDir: string) {
  // Mock → click Browse for input
  await mockNextDialog(app, inputDir);
  await page.getByTestId('input-dir-browse').click();
  await expect(page.getByTestId('input-dir-field')).toHaveValue(inputDir, { timeout: 5_000 });

  // Mock → click Browse for output
  await mockNextDialog(app, outputDir);
  await page.getByTestId('output-dir-browse').click();
  await expect(page.getByTestId('output-dir-field')).toHaveValue(outputDir, { timeout: 5_000 });

  await expect(page.getByTestId('start-btn')).toBeEnabled({ timeout: 5_000 });
}

// start-btn is disabled while running, re-enables when done
async function waitForDone(page: Page, timeoutMs = 900_000) {
  await expect(page.getByTestId('start-btn')).toBeDisabled({ timeout: 15_000 });
  await expect(page.getByTestId('start-btn')).toBeEnabled({ timeout: timeoutMs });
}

// ---------------------------------------------------------------------------
// TEST 2: Float scale 1.5× — MaxQuality
// ---------------------------------------------------------------------------
test('Test 2: GUI float scale 1.5× upscale (MaxQuality)', async () => {
  const userDataDir = path.join(os.tmpdir(), `dlss-pw-real-1.5x-${Date.now()}`);
  const { app, page } = await launchApp(userDataDir);

  try {
    const scaleInput = page.getByTestId('scale-input');
    await scaleInput.fill('1.5');
    await scaleInput.press('Tab');
    await expect(scaleInput).toHaveValue('1.5');

    await page.getByTestId('quality-select').selectOption('MaxQuality');
    await configureIO(app, page, INPUT_DIR, OUT_1_5x);

    await page.screenshot({ path: path.join(EVIDENCE, 'test2-gui-1.5x-before.png') });

    await page.getByTestId('start-btn').click();
    await expect(page.getByTestId('log-output')).toBeVisible({ timeout: 10_000 });

    await page.waitForTimeout(5_000);
    await page.screenshot({ path: path.join(EVIDENCE, 'test2-gui-1.5x-progress.png') });

    await waitForDone(page, 600_000);

    await page.screenshot({ path: path.join(EVIDENCE, 'test2-gui-1.5x-done.png') });

    const files = fs.readdirSync(OUT_1_5x).filter(f => f.endsWith('.exr'));
    console.log(`Test 2 output: ${files.length} EXR files in ${OUT_1_5x}`);
    expect(files.length).toBe(1000);

  } finally {
    await app.close();
    fs.rmSync(userDataDir, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------------------
// TEST 3: Standard 2× upscale (default settings)
// ---------------------------------------------------------------------------
test('Test 3: GUI standard 2× upscale', async () => {
  const userDataDir = path.join(os.tmpdir(), `dlss-pw-real-2x-${Date.now()}`);
  const { app, page } = await launchApp(userDataDir);

  try {
    await expect(page.getByTestId('scale-input')).toHaveValue('2');
    await expect(page.getByTestId('quality-select')).toHaveValue('MaxQuality');

    await configureIO(app, page, INPUT_DIR, OUT_2x);

    await page.screenshot({ path: path.join(EVIDENCE, 'test3-gui-2x-before.png') });

    await page.getByTestId('start-btn').click();
    await expect(page.getByTestId('log-output')).toBeVisible({ timeout: 10_000 });

    await page.waitForTimeout(5_000);
    await page.screenshot({ path: path.join(EVIDENCE, 'test3-gui-2x-progress.png') });

    await waitForDone(page, 600_000);

    await page.screenshot({ path: path.join(EVIDENCE, 'test3-gui-2x-done.png') });

    const files = fs.readdirSync(OUT_2x).filter(f => f.endsWith('.exr'));
    console.log(`Test 3 output: ${files.length} EXR files in ${OUT_2x}`);
    expect(files.length).toBe(1000);

  } finally {
    await app.close();
    fs.rmSync(userDataDir, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------------------
// TEST 4: Combined RR+FG 2× (scale 2 + interpolate 2x)
// ---------------------------------------------------------------------------
test('Test 4: GUI combined RR+FG 2× (scale 2 + interpolate 2x)', async () => {
  const userDataDir = path.join(os.tmpdir(), `dlss-pw-real-rrfg-${Date.now()}`);
  const { app, page } = await launchApp(userDataDir);

  try {
    await expect(page.getByTestId('scale-input')).toHaveValue('2');

    await page.getByTestId('interpolation-select').selectOption('2');

    const cameraPicker = page.getByTestId('camera-data-picker');
    await expect(cameraPicker).toBeEnabled({ timeout: 5_000 });
    await mockNextDialog(app, CAMERA_JSON);
    await cameraPicker.click();

    await configureIO(app, page, INPUT_DIR, OUT_RRFG);

    await page.screenshot({ path: path.join(EVIDENCE, 'test4-gui-rrfg-before.png') });

    await page.getByTestId('start-btn').click();
    await expect(page.getByTestId('log-output')).toBeVisible({ timeout: 10_000 });

    await page.waitForTimeout(5_000);
    await page.screenshot({ path: path.join(EVIDENCE, 'test4-gui-rrfg-progress.png') });

    await waitForDone(page, 900_000);

    await page.screenshot({ path: path.join(EVIDENCE, 'test4-gui-rrfg-done.png') });

    const files = fs.readdirSync(OUT_RRFG).filter(f => f.endsWith('.exr'));
    console.log(`Test 4 output: ${files.length} EXR files in ${OUT_RRFG}`);
    expect(files.length).toBeGreaterThanOrEqual(1999);

  } finally {
    await app.close();
    fs.rmSync(userDataDir, { recursive: true, force: true });
  }
});
