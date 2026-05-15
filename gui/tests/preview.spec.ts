/**
 * Coverage for the frame-preview area in ProcessingView.
 * In the default (idle) state we expect:
 *   - The frame-preview container is present
 *   - The "Ready to process" hint is visible
 *   - The Start button is rendered (disabled because no exe / dirs configured)
 *
 * The frame-preview-placeholder and frame-preview-img only appear once
 * processing starts (status='running'|'done'); those paths require a real
 * dlss-compositor.exe and are covered by real-processing.spec.ts.
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
const userDataDir = path.join(os.tmpdir(), `dlss-pw-preview-${Date.now()}`);

test.beforeAll(async () => {
  fs.rmSync(path.resolve(__dirname, '..', 'dlss-compositor.json'), { force: true });
  app = await electron.launch({
    executablePath: require('electron') as unknown as string,
    args: [mainEntry, `--user-data-dir=${userDataDir}`],
  });
  page = await app.firstWindow();
  await page.waitForLoadState('domcontentloaded');
  await page.getByTestId('main-area').waitFor({ state: 'visible', timeout: 15_000 });
});

test.afterAll(async () => {
  await app?.close();
  fs.rmSync(userDataDir, { recursive: true, force: true });
});

test('frame-preview container is present in the main area', async () => {
  const preview = page.getByTestId('frame-preview');
  await expect(preview).toBeVisible();
});

test('idle state shows "Ready to process" hint inside the preview area', async () => {
  const preview = page.getByTestId('frame-preview');
  await expect(preview).toContainText(/Ready to process/i);
});

test('start button is rendered (disabled) on a fresh launch', async () => {
  const startBtn = page.getByTestId('start-btn');
  await expect(startBtn).toBeVisible();
  await expect(startBtn).toBeDisabled();
});

test('status indicator starts in idle state', async () => {
  const indicator = page.getByTestId('status-indicator');
  await expect(indicator).toBeVisible();
  await expect(indicator).toHaveAttribute('data-status', 'idle');
});

test('progress bar is hidden while idle', async () => {
  // ProgressBar is conditionally rendered only when running or done with totalFrames
  await expect(page.getByTestId('progress-bar')).toHaveCount(0);
});

test('preview area takes up significant vertical space (flex-1)', async () => {
  // Sanity check: the preview should occupy the bulk of the main area,
  // confirming Phase 4 layout wasn't accidentally collapsed.
  const previewBox = await page.getByTestId('frame-preview').boundingBox();
  expect(previewBox).not.toBeNull();
  expect(previewBox!.height).toBeGreaterThan(100);
});
