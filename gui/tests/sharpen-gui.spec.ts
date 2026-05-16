/**
 * GUI sharpness crash regression test.
 * Verifies sharpness > 0 does NOT crash CLI when invoked via the Electron GUI.
 * Test A: SR-only + sharpness=0.15
 * Test B: SR+FG 2x + sharpness=0.15  (user's exact failing config)
 */

import { test, expect, type ElectronApplication, type Page } from '@playwright/test';
import { _electron as electron } from 'playwright';
import path from 'node:path';
import os from 'node:os';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const __filename = fileURLToPath(import.meta.url);
const __dirname  = path.dirname(__filename);
const require    = createRequire(import.meta.url);

const mainEntry   = path.resolve(__dirname, '..', 'dist-electron', 'main.js');
const INPUT_DIR   = path.resolve(__dirname, '..', '..', 'build', 'Release', 'tmp', 'real-test');
const CAMERA_JSON = path.join(INPUT_DIR, 'camera.json');

async function launchApp(userDataDir: string): Promise<{ app: ElectronApplication; page: Page }> {
  fs.mkdirSync(userDataDir, { recursive: true });
  fs.rmSync(path.resolve(__dirname, '..', 'dlss-compositor.json'), { force: true });
  const app = await electron.launch({
    executablePath: require('electron') as unknown as string,
    args: [mainEntry, `--user-data-dir=${userDataDir}`],
  });
  const page = await app.firstWindow();
  await page.waitForLoadState('domcontentloaded');
  await page.getByTestId('settings-panel').waitFor({ state: 'visible', timeout: 15_000 });
  return { app, page };
}

async function mockDialog(app: ElectronApplication, returnPath: string) {
  await app.evaluate(({ ipcMain }, p) => {
    ipcMain.removeHandler('dialog:openDirectory');
    ipcMain.removeHandler('dialog:openFile');
    ipcMain.handleOnce('dialog:openDirectory', () => p);
    ipcMain.handleOnce('dialog:openFile',      () => p);
  }, returnPath);
}

/** Set a React-controlled range slider via native setter + bubbling events. */
async function setSlider(page: Page, testId: string, value: number) {
  await page.getByTestId(testId).evaluate((el, val) => {
    const input = el as HTMLInputElement;
    const setter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value')!.set!;
    setter.call(input, String(val));
    input.dispatchEvent(new Event('input',  { bubbles: true }));
    input.dispatchEvent(new Event('change', { bubbles: true }));
  }, value);
}

async function waitForDone(page: Page, timeoutMs = 90_000) {
  await expect(page.getByTestId('start-btn')).toBeDisabled({ timeout: 20_000 });
  await expect(page.getByTestId('start-btn')).toBeEnabled({ timeout: timeoutMs });
}

// ---------------------------------------------------------------------------
// Test A: SR-only + sharpness=0.15
// ---------------------------------------------------------------------------
test('GUI SR + sharpness=0.15 exits clean', async () => {
  test.setTimeout(120_000);
  const outDir      = path.join(os.tmpdir(), `dlss-gui-sharp-sr-${Date.now()}`);
  const userDataDir = path.join(os.tmpdir(), `dlss-pw-sharp-sr-${Date.now()}`);
  fs.mkdirSync(outDir, { recursive: true });
  const { app, page } = await launchApp(userDataDir);
  try {
    await mockDialog(app, INPUT_DIR);
    await page.getByTestId('input-dir-browse').click();
    await expect(page.getByTestId('input-dir-field')).toHaveValue(INPUT_DIR, { timeout: 8_000 });

    await mockDialog(app, outDir);
    await page.getByTestId('output-dir-browse').click();
    await expect(page.getByTestId('output-dir-field')).toHaveValue(outDir, { timeout: 5_000 });

    await page.getByTestId('quality-select').selectOption('Performance');
    await setSlider(page, 'sharpness-slider', 0.15);
    await expect(page.getByTestId('sharpness-slider')).toHaveValue('0.15');

    await expect(page.getByTestId('start-btn')).toBeEnabled({ timeout: 5_000 });
    await page.getByTestId('start-btn').click();
    await waitForDone(page);

    const hasError = await page.getByTestId('error-banner').isVisible().catch(() => false);
    if (hasError) {
      const msg = await page.getByTestId('error-banner').textContent().catch(() => '');
      throw new Error(`CLI crashed: ${msg}`);
    }
    const written = fs.readdirSync(outDir).filter(f => f.endsWith('.exr'));
    console.log(`[SR+sharpen] wrote ${written.length} EXR files`);
    expect(written.length).toBeGreaterThan(0);
  } finally {
    await app.close();
    fs.rmSync(userDataDir, { recursive: true, force: true });
    fs.rmSync(outDir,      { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------------------
// Test B: SR + FG 2x + sharpness=0.15  (user's exact failing config)
// ---------------------------------------------------------------------------
test('GUI SR+FG 2x + sharpness=0.15 exits clean', async () => {
  test.setTimeout(120_000);
  const outDir      = path.join(os.tmpdir(), `dlss-gui-sharp-srfg-${Date.now()}`);
  const userDataDir = path.join(os.tmpdir(), `dlss-pw-sharp-srfg-${Date.now()}`);
  fs.mkdirSync(outDir, { recursive: true });
  const { app, page } = await launchApp(userDataDir);
  try {
    await mockDialog(app, INPUT_DIR);
    await page.getByTestId('input-dir-browse').click();
    await expect(page.getByTestId('input-dir-field')).toHaveValue(INPUT_DIR, { timeout: 8_000 });

    await mockDialog(app, outDir);
    await page.getByTestId('output-dir-browse').click();
    await expect(page.getByTestId('output-dir-field')).toHaveValue(outDir, { timeout: 5_000 });

    await page.getByTestId('quality-select').selectOption('Performance');
    await page.getByTestId('interpolation-select').selectOption('2');

    await mockDialog(app, CAMERA_JSON);
    await page.getByTestId('camera-data-picker').click();
    await page.waitForTimeout(1_000);

    await setSlider(page, 'sharpness-slider', 0.15);
    await expect(page.getByTestId('sharpness-slider')).toHaveValue('0.15');

    await expect(page.getByTestId('start-btn')).toBeEnabled({ timeout: 5_000 });
    await page.getByTestId('start-btn').click();
    await waitForDone(page);

    const hasError = await page.getByTestId('error-banner').isVisible().catch(() => false);
    if (hasError) {
      const msg = await page.getByTestId('error-banner').textContent().catch(() => '');
      throw new Error(`CLI crashed: ${msg}`);
    }
    const written = fs.readdirSync(outDir).filter(f => f.endsWith('.exr'));
    console.log(`[SRFG+sharpen] wrote ${written.length} EXR files`);
    expect(written.length).toBeGreaterThan(0);
  } finally {
    await app.close();
    fs.rmSync(userDataDir, { recursive: true, force: true });
    fs.rmSync(outDir,      { recursive: true, force: true });
  }
});
