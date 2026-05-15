/**
 * T15 ProcessingView contract:
 *   - Idle state contains zero progress-bar testids (the duplicate progress
 *     bar inside ProcessingView was removed; the single progress signal lives
 *     elsewhere).
 *   - The structured FFmpeg preview-error testids exist in the source but are
 *     gated behind status === 'running'/'done' AND a previewError state, so
 *     the idle launch should expose none of them.
 *   - The frame-preview area itself is always rendered.
 *   - Start button is disabled because no input/output dirs are configured on
 *     a clean launch.
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
const userDataDir = path.join(os.tmpdir(), `dlss-pw-prevuierr-${Date.now()}`);

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

test('idle ProcessingView has zero progress-bar elements', async () => {
  // T15 removed the duplicate progress-bar testid from ProcessingView.
  await expect(page.getByTestId('progress-bar')).toHaveCount(0);
});

test('frame-preview area renders in idle state', async () => {
  await expect(page.getByTestId('frame-preview')).toBeVisible();
  await expect(page.getByTestId('frame-preview')).toContainText(/Ready to process/i);
});

test('start button is rendered but disabled on clean launch', async () => {
  const startBtn = page.getByTestId('start-btn');
  await expect(startBtn).toBeVisible();
  await expect(startBtn).toBeDisabled();
});

test('structured preview-error testids are absent in idle state', async () => {
  // Each preview-error variant is gated on previewError state — none should
  // be present until a frame is processed and FFmpeg fails.
  await expect(page.getByTestId('preview-error-decode')).toHaveCount(0);
  await expect(page.getByTestId('preview-error-timeout')).toHaveCount(0);
  await expect(page.getByTestId('preview-error-unknown')).toHaveCount(0);
  // ffmpeg variant may be present if the early FFmpeg-status ping marks it
  // unavailable; only assert it's not rendered more than once.
  const ffmpegError = page.getByTestId('preview-error-ffmpeg');
  expect(await ffmpegError.count()).toBeLessThanOrEqual(1);
});

test('frame-preview-img is not rendered before any frame is written', async () => {
  await expect(page.getByTestId('frame-preview-img')).toHaveCount(0);
});

test('status indicator starts in idle data-status', async () => {
  const indicator = page.getByTestId('status-indicator');
  await expect(indicator).toBeVisible();
  await expect(indicator).toHaveAttribute('data-status', 'idle');
});

test('error-banner is absent in idle state', async () => {
  // The error-banner only renders when status === 'error'.
  await expect(page.getByTestId('error-banner')).toHaveCount(0);
});
