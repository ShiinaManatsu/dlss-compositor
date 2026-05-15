/**
 * Coverage for the Phase 4 controls (post-T7 restructure):
 *   - source-fps input lives in SRSettings (always visible)
 *   - sharpness slider lives in SRSettings (always visible)
 *   - ngx-log-level select lives in CommonSettings (always visible)
 *   - tonemap select lives in FGSettings with custom option (always visible)
 *   - tonemap=custom + missing forward LUT keeps Start disabled
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
const userDataDir = path.join(os.tmpdir(), `dlss-pw-newctrls-${Date.now()}`);

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

test('source-fps input is present with default value 30', async () => {
  const input = page.getByTestId('source-fps-input');
  await expect(input).toBeVisible();
  await expect(input).toHaveValue('30');
});

test('source-fps input clamps out-of-range values on blur', async () => {
  const input = page.getByTestId('source-fps-input');
  await input.fill('500');
  await input.press('Tab');
  // Clamp max is 240
  await expect(input).toHaveValue('240');
  // Reset
  await input.fill('30');
  await input.press('Tab');
});

test('sharpness slider defaults to 0 and is always visible', async () => {
  const slider = page.getByTestId('sharpness-slider');
  await expect(slider).toBeVisible();
  await expect(slider).toHaveValue('0');
});

test('ngx-log-level select defaults to warn and is always visible', async () => {
  const select = page.getByTestId('ngx-log-level-select');
  await expect(select).toBeVisible();
  await expect(select).toHaveValue('warn');
  const optionValues = await select.locator('option').evaluateAll((opts) =>
    (opts as HTMLOptionElement[]).map((o) => o.value),
  );
  expect(optionValues).toEqual(['off', 'error', 'warn', 'info', 'verbose']);
});

test('tonemap select has pq, none, and custom options', async () => {
  const select = page.getByTestId('tonemap-select');
  await expect(select).toBeVisible();
  const optionValues = await select.locator('option').evaluateAll((opts) =>
    (opts as HTMLOptionElement[]).map((o) => o.value),
  );
  expect(optionValues).toEqual(['pq', 'none', 'custom']);
});

test('selecting tonemap=custom enables forward-LUT picker', async () => {
  await page.getByTestId('tonemap-select').selectOption('custom');
  await expect(page.getByTestId('forward-lut-picker')).toBeEnabled();
  // Reset
  await page.getByTestId('tonemap-select').selectOption('pq');
});

test('tonemap=custom + missing forward LUT keeps start button disabled', async () => {
  await page.getByTestId('tonemap-select').selectOption('custom');
  // Without an exe path / input dir / output dir / forward LUT, start is disabled.
  await expect(page.getByTestId('start-btn')).toBeDisabled();
  // Reset
  await page.getByTestId('tonemap-select').selectOption('pq');
});
