/**
 * Coverage for ResolutionPanel:
 *   - target preset select (4K / 1440p / 1080p / DLAA / Custom)
 *   - source-resolution display defaults to "Unknown" when no camera.json
 *   - Custom preset reveals width/height inputs
 *   - computed-scale-display appears for DLAA (always known scale=1.0)
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
const userDataDir = path.join(os.tmpdir(), `dlss-pw-resolution-${Date.now()}`);

test.beforeAll(async () => {
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

test('target preset select is present and visible', async () => {
  const select = page.getByTestId('target-preset-select');
  await expect(select).toBeVisible();
});

test('target preset select offers 4K / 1440p / 1080p / DLAA / Custom', async () => {
  const select = page.getByTestId('target-preset-select');
  const optionValues = await select.locator('option').evaluateAll((opts) =>
    (opts as HTMLOptionElement[]).map((o) => o.value),
  );
  expect(optionValues).toEqual(['4K', '1440p', '1080p', 'DLAA', 'Custom']);
});

test('source resolution display shows Unknown when no camera.json was loaded', async () => {
  const display = page.getByTestId('source-resolution-display');
  await expect(display).toBeVisible();
  await expect(display).toContainText(/Unknown/);
});

test('selecting Custom preset reveals custom width and height inputs', async () => {
  const select = page.getByTestId('target-preset-select');
  await select.selectOption('Custom');
  await expect(page.getByTestId('custom-target-width')).toBeVisible();
  await expect(page.getByTestId('custom-target-height')).toBeVisible();
  // Reset
  await select.selectOption('1080p');
  await expect(page.getByTestId('custom-target-width')).toHaveCount(0);
});

test('computed scale display is shown for DLAA (always known)', async () => {
  const select = page.getByTestId('target-preset-select');
  await select.selectOption('DLAA');
  const scale = page.getByTestId('computed-scale-display');
  await expect(scale).toBeVisible();
  await expect(scale).toContainText(/Scale:\s*1\.00/);
  // Reset
  await select.selectOption('1080p');
});
