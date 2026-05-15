/**
 * T7 sidebar restructure contract:
 *   - Three independent always-visible sections: Common Settings, SR Settings,
 *     FG Settings (the legacy BasicSettings.tsx is gone).
 *   - SR-specific controls (quality, preset, sharpness, source FPS, advanced
 *     NGX) live under SR Settings.
 *   - FG-specific controls (interpolation, camera data, tonemap, LUT pickers,
 *     dynamic resolution toggle) live under FG Settings.
 *   - Shared concerns (memory budget, EXR compression, video output, NGX log
 *     level, channel map) live under Common Settings.
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
const userDataDir = path.join(os.tmpdir(), `dlss-pw-srfgsep-${Date.now()}`);

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

test('Common Settings section header is rendered', async () => {
  await expect(page.getByText('Common Settings')).toBeVisible();
});

test('SR Settings section header is rendered', async () => {
  await expect(page.getByText('SR Settings')).toBeVisible();
});

test('FG Settings section header is rendered', async () => {
  await expect(page.getByText('FG Settings')).toBeVisible();
});

test('SR Settings hosts the quality-select control', async () => {
  await expect(page.getByTestId('quality-select')).toBeVisible();
});

test('SR Settings hosts the SR scale input', async () => {
  // SRSettings's scale-input is the canonical scale control.
  await expect(page.getByTestId('scale-input')).toBeVisible();
});

test('SR Settings hosts the source-fps input', async () => {
  await expect(page.getByTestId('source-fps-input')).toBeVisible();
});

test('SR Settings hosts the sharpness slider', async () => {
  await expect(page.getByTestId('sharpness-slider')).toBeVisible();
});

test('FG Settings hosts the interpolation-select control', async () => {
  await expect(page.getByTestId('interpolation-select')).toBeVisible();
});

test('FG Settings hosts the camera-data-picker', async () => {
  await expect(page.getByTestId('camera-data-picker')).toBeVisible();
});

test('FG Settings hosts the tonemap-select control', async () => {
  await expect(page.getByTestId('tonemap-select')).toBeVisible();
});

test('Common Settings hosts the memory-slider control', async () => {
  await expect(page.getByTestId('memory-slider')).toBeVisible();
});

test('Common Settings hosts the EXR compression select', async () => {
  await expect(page.getByTestId('exr-compression-select')).toBeVisible();
});

test('Common Settings hosts the ngx-log-level select', async () => {
  await expect(page.getByTestId('ngx-log-level-select')).toBeVisible();
});

test('legacy advanced-settings-toggle from BasicSettings is gone', async () => {
  // The basic/advanced split was deleted in T7. The Advanced NGX panel uses
  // a different testid (advanced-ngx-toggle).
  await expect(page.getByTestId('advanced-settings-toggle')).toHaveCount(0);
  await expect(page.getByTestId('advanced-settings-content')).toHaveCount(0);
});
