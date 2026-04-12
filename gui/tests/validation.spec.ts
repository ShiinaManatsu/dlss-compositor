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
const userDataDir = path.join(os.tmpdir(), `dlss-pw-validation-${Date.now()}`);

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

test('start button is disabled when no input dir set', async () => {
  // Fresh launch has no inputDir → start should be disabled
  await expect(page.getByTestId('start-btn')).toBeDisabled();
});

test('start button is disabled when no exe path configured', async () => {
  // Fresh launch has no exePath → start should be disabled
  await expect(page.getByTestId('start-btn')).toBeDisabled();
});

test('input directory shows error on blur when empty', async () => {
  const inputField = page.getByTestId('input-dir-field');
  await inputField.focus();
  await inputField.blur();

  await expect(page.getByTestId('input-dir-error')).toBeVisible();
  await expect(page.getByTestId('input-dir-error')).toContainText('required');
});
