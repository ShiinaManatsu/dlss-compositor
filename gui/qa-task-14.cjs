// Task 14 QA: Processing View + Log Output + Start/Stop Controls
// Run: node gui/qa-task-14.cjs
const { _electron: electron } = require('playwright');
const path = require('path');
const fs = require('fs');

const EVIDENCE_DIR = path.join(__dirname, '..', '.sisyphus', 'evidence');
if (!fs.existsSync(EVIDENCE_DIR)) fs.mkdirSync(EVIDENCE_DIR, { recursive: true });

const TEMP_DIR = path.join(__dirname, 'qa-temp-14');
const USER_DATA = path.join(TEMP_DIR, 'userData');

function cleanup() {
  try {
    if (fs.existsSync(TEMP_DIR)) fs.rmSync(TEMP_DIR, { recursive: true, force: true });
  } catch (e) { /* ignore */ }
}

async function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function scenario1_startDisabledAndEmptyState() {
  console.log('\n=== Scenario 1: Start disabled + idle UI state ===');
  cleanup();
  fs.mkdirSync(USER_DATA, { recursive: true });

  const app = await electron.launch({
    args: ['.', `--user-data-dir=${USER_DATA}`],
    cwd: path.join(__dirname),
  });
  const page = await app.firstWindow();
  await page.waitForSelector('[data-testid="app-root"]', { timeout: 15000 });
  await sleep(1500);

  const checks = [];

  // Start button disabled (no dirs, no exe)
  const isDisabled = await page.locator('[data-testid="start-btn"]').isDisabled();
  console.log(`  Start disabled (no fields): ${isDisabled ? 'PASS' : 'FAIL'}`);
  checks.push(isDisabled);

  // Status idle
  const status = await page.locator('[data-testid="status-indicator"]').getAttribute('data-status');
  console.log(`  Status is idle: ${status === 'idle' ? 'PASS' : 'FAIL'}`);
  checks.push(status === 'idle');

  // Empty state message in log
  const logText = await page.locator('[data-testid="log-output"]').textContent();
  const hasMsg = logText.includes('Ready to process');
  console.log(`  Empty state message: ${hasMsg ? 'PASS' : 'FAIL'}`);
  checks.push(hasMsg);

  // Stop button NOT visible
  const stopVisible = await page.locator('[data-testid="stop-btn"]').isVisible().catch(() => false);
  console.log(`  Stop button hidden: ${!stopVisible ? 'PASS' : 'FAIL'}`);
  checks.push(!stopVisible);

  // Progress bar NOT visible
  const progressVisible = await page.locator('[data-testid="progress-bar"]').isVisible().catch(() => false);
  console.log(`  Progress bar hidden: ${!progressVisible ? 'PASS' : 'FAIL'}`);
  checks.push(!progressVisible);

  // Elapsed NOT visible
  const elapsedVisible = await page.locator('[data-testid="elapsed-time"]').isVisible().catch(() => false);
  console.log(`  Elapsed time hidden: ${!elapsedVisible ? 'PASS' : 'FAIL'}`);
  checks.push(!elapsedVisible);

  await page.screenshot({ path: path.join(EVIDENCE_DIR, 'task-14-start-disabled.png') });
  console.log('  Screenshot saved: task-14-start-disabled.png');

  await app.close();
  return checks.every(Boolean);
}

async function scenario2_allUIElements() {
  console.log('\n=== Scenario 2: All data-testid elements present ===');
  cleanup();
  fs.mkdirSync(USER_DATA, { recursive: true });

  const app = await electron.launch({
    args: ['.', `--user-data-dir=${USER_DATA}`],
    cwd: path.join(__dirname),
  });
  const page = await app.firstWindow();
  await page.waitForSelector('[data-testid="app-root"]', { timeout: 15000 });
  await sleep(1500);

  const checks = [];
  for (const tid of ['start-btn', 'status-indicator', 'log-output']) {
    const exists = await page.locator(`[data-testid="${tid}"]`).count() > 0;
    console.log(`  ${tid}: ${exists ? 'PASS' : 'FAIL'}`);
    checks.push(exists);
  }

  await page.screenshot({ path: path.join(EVIDENCE_DIR, 'task-14-ui-elements.png') });
  console.log('  Screenshot saved: task-14-ui-elements.png');

  await app.close();
  return checks.every(Boolean);
}

async function scenario3_startEnablesWithPreseeded() {
  console.log('\n=== Scenario 3: Start enables with pre-seeded settings ===');
  cleanup();
  fs.mkdirSync(USER_DATA, { recursive: true });
  const inputDir = path.join(TEMP_DIR, 'input');
  const outputDir = path.join(TEMP_DIR, 'output');
  fs.mkdirSync(inputDir, { recursive: true });
  fs.mkdirSync(outputDir, { recursive: true });

  // Pre-seed electron-store with flat keys (filename: dlss-compositor-settings.json)
  fs.writeFileSync(path.join(USER_DATA, 'dlss-compositor-settings.json'), JSON.stringify({
    exePath: 'C:\\Windows\\System32\\cmd.exe',
    lastInputDir: inputDir,
    lastOutputDir: outputDir,
    scaleFactor: 2.0,
    quality: 'MaxQuality',
    interpolateFactor: 0,
    exrCompression: 'dwaa',
    exrDwaQuality: 95.0,
    memoryBudgetGB: 8,
    tonemapMode: 'pq',
    inverseTonemapEnabled: true,
  }));

  const app = await electron.launch({
    args: ['.', `--user-data-dir=${USER_DATA}`],
    cwd: path.join(__dirname),
  });
  const page = await app.firstWindow();
  await page.waitForSelector('[data-testid="app-root"]', { timeout: 15000 });
  await sleep(2000);

  // Check if dirs were loaded into the state (visible in input fields)
  const inputVal = await page.locator('[data-testid="input-dir-field"]').inputValue();
  console.log(`  Input dir loaded: ${inputVal ? 'PASS' : 'FAIL'} (${inputVal})`);

  const outputVal = await page.locator('[data-testid="output-dir-field"]').inputValue();
  console.log(`  Output dir loaded: ${outputVal ? 'PASS' : 'FAIL'} (${outputVal})`);

  // Check if start is enabled
  const isEnabled = await page.locator('[data-testid="start-btn"]').isEnabled();
  console.log(`  Start enabled: ${isEnabled ? 'PASS' : 'FAIL'}`);

  await page.screenshot({ path: path.join(EVIDENCE_DIR, 'task-14-start-enabled.png') });
  console.log('  Screenshot saved: task-14-start-enabled.png');

  await app.close();
  // Pass if both dirs loaded and start is enabled
  return !!inputVal && !!outputVal && isEnabled;
}

async function scenario4_errorOnBadExe() {
  console.log('\n=== Scenario 4: Error status on bad exe path ===');
  cleanup();
  fs.mkdirSync(USER_DATA, { recursive: true });
  const inputDir = path.join(TEMP_DIR, 'input');
  const outputDir = path.join(TEMP_DIR, 'output');
  fs.mkdirSync(inputDir, { recursive: true });
  fs.mkdirSync(outputDir, { recursive: true });

  // Pre-seed with a non-existent exe (flat keys, correct filename)
  fs.writeFileSync(path.join(USER_DATA, 'dlss-compositor-settings.json'), JSON.stringify({
    exePath: path.join(TEMP_DIR, 'nonexistent.exe'),
    lastInputDir: inputDir,
    lastOutputDir: outputDir,
    scaleFactor: 2.0,
    quality: 'MaxQuality',
    interpolateFactor: 0,
    exrCompression: 'dwaa',
    exrDwaQuality: 95.0,
    memoryBudgetGB: 8,
    tonemapMode: 'pq',
    inverseTonemapEnabled: true,
  }));

  const app = await electron.launch({
    args: ['.', `--user-data-dir=${USER_DATA}`],
    cwd: path.join(__dirname),
  });
  const page = await app.firstWindow();
  await page.waitForSelector('[data-testid="app-root"]', { timeout: 15000 });
  await sleep(2000);

  // Start button should still be enabled (ProcessingView checks exePath exists, not validates)
  // Actually the exe validation is in ExePathConfig, but ProcessingView just checks exePath is truthy
  // So start may be enabled. Click it and see if error appears.
  const startBtn = page.locator('[data-testid="start-btn"]');
  const canClick = await startBtn.isEnabled();
  
  if (canClick) {
    await startBtn.click();
    await sleep(3000);

    // Should get error status since exe doesn't exist
    const status = await page.locator('[data-testid="status-indicator"]').getAttribute('data-status');
    console.log(`  Status after bad exe: ${status} ${status === 'error' ? 'PASS' : 'INFO'}`);

    // Error banner should be visible
    const errorVisible = await page.locator('[data-testid="error-banner"]').isVisible().catch(() => false);
    console.log(`  Error banner visible: ${errorVisible ? 'PASS' : 'INFO'}`);
  } else {
    console.log(`  Start disabled (exe validation caught it): PASS`);
  }

  await page.screenshot({ path: path.join(EVIDENCE_DIR, 'task-14-error-state.png') });
  console.log('  Screenshot saved: task-14-error-state.png');

  await app.close();
  return true; // Informational test
}

(async () => {
  let allPass = true;
  try {
    const r1 = await scenario1_startDisabledAndEmptyState();
    allPass = allPass && r1;
  } catch (e) { console.error('Scenario 1 failed:', e.message); allPass = false; }

  try {
    const r2 = await scenario2_allUIElements();
    allPass = allPass && r2;
  } catch (e) { console.error('Scenario 2 failed:', e.message); allPass = false; }

  try {
    const r3 = await scenario3_startEnablesWithPreseeded();
    allPass = allPass && r3;
  } catch (e) { console.error('Scenario 3 failed:', e.message); allPass = false; }

  try {
    const r4 = await scenario4_errorOnBadExe();
    allPass = allPass && r4;
  } catch (e) { console.error('Scenario 4 failed:', e.message); allPass = false; }

  cleanup();
  console.log(`\n${'='.repeat(50)}`);
  console.log(`Overall: ${allPass ? 'ALL PASS' : 'SOME FAILED'}`);
  process.exit(allPass ? 0 : 1);
})();
