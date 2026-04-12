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
const userDataDir = path.join(os.tmpdir(), `dlss-pw-launch-${Date.now()}`);

test.beforeAll(async () => {
  app = await electron.launch({
    executablePath: require('electron') as unknown as string,
    args: [mainEntry, `--user-data-dir=${userDataDir}`],
  });
  page = await app.firstWindow();
  await page.waitForLoadState('domcontentloaded');
});

test.afterAll(async () => {
  await app?.close();
  fs.rmSync(userDataDir, { recursive: true, force: true });
});

test('window title is DLSS Compositor', async () => {
  const title = await page.title();
  expect(title).toContain('DLSS Compositor');
});

test('layout: settings panel and main area visible', async () => {
  await expect(page.getByTestId('settings-panel')).toBeVisible();
  await expect(page.getByTestId('main-area')).toBeVisible();
});

test('default settings: scale=2.0, quality=MaxQuality, interpolation=None', async () => {
  const scaleInput = page.getByTestId('scale-input');
  await expect(scaleInput).toHaveValue('2');

  const qualitySelect = page.getByTestId('quality-select');
  await expect(qualitySelect).toHaveValue('MaxQuality');

  const interpolationSelect = page.getByTestId('interpolation-select');
  await expect(interpolationSelect).toHaveValue('0');
});
