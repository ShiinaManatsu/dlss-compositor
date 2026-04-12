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
const userDataDir = path.join(os.tmpdir(), `dlss-pw-settings-${Date.now()}`);

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

test('scale input accepts float values', async () => {
  const scaleInput = page.getByTestId('scale-input');
  await scaleInput.fill('1.5');
  await scaleInput.press('Tab');

  await expect(scaleInput).toHaveValue('1.5');
});

test('quality can be manually selected', async () => {
  await page.getByTestId('quality-select').selectOption('Balanced');
  await expect(page.getByTestId('quality-select')).toHaveValue('Balanced');

  // Reset
  await page.getByTestId('quality-select').selectOption('MaxQuality');
});

test('advanced settings panel toggles open and closed', async () => {
  const content = page.getByTestId('advanced-settings-content');
  // Initially hidden
  await expect(content).toHaveClass(/hidden/);

  await page.getByTestId('advanced-settings-toggle').click();
  await expect(content).not.toHaveClass(/hidden/);

  await page.getByTestId('advanced-settings-toggle').click();
  await expect(content).toHaveClass(/hidden/);
});

test('interpolation→2x enables camera data picker', async () => {
  const picker = page.getByTestId('camera-data-picker');
  // Default interpolation = 0 → picker disabled
  await expect(picker).toBeDisabled();

  await page.getByTestId('interpolation-select').selectOption('2');
  await expect(picker).toBeEnabled();

  // Reset to 0
  await page.getByTestId('interpolation-select').selectOption('0');
});
