import {app, BrowserWindow, dialog, ipcMain, shell} from 'electron';
import {execSync, spawn} from 'node:child_process';
import fs from 'node:fs/promises';
import fsSync from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {PipelineOrchestrator, configureOrchestratorPaths, parseCliFlags, type ExportRequest, type SceneOverrides} from './orchestrator';
import {
  ensureDcpExtension,
  fileExists,
  listRecent,
  loadDcpFile,
  rememberRecent,
  saveDcpFile,
} from './project';
import {
  DEFAULT_FILENAME_TEMPLATE,
  jobTokenFromBlend,
  parseFg,
  resolveFilenameTemplate,
  type DcpProject,
} from '../src/dcp';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
// Packaged builds resolve the bundled backend (cli + pipeline scripts) under
// resources/ and keep logs in userData; dev builds stay inside the repo.
const packaged = app.isPackaged;
const logDir = packaged
  ? path.join(app.getPath('userData'), 'logs')
  : path.resolve(__dirname, '../../build/stage4-out');
if (packaged) {
  configureOrchestratorPaths({
    backend: path.join(process.resourcesPath, 'backend'),
    logs: logDir,
  });
}
const orchestrator = new PipelineOrchestrator(packaged || process.env.DC_RAMDISK_AUTO === '1');

const flags = parseCliFlags(process.argv);
// Installer file association launches as `DLSS Compositor.exe C:\path\job.dcp`
// — the bare path sits at argv[1], outside parseCliFlags' flag list.
if (!flags.dcp) {
  const dcpArg = process.argv.slice(1).find((item) => /\.dcp$/i.test(item));
  if (dcpArg && fsSync.existsSync(dcpArg)) flags.dcp = dcpArg;
}
const debugLog = path.join(logDir, 'app-debug.log');
app.commandLine.appendSwitch('disable-gpu-sandbox');
app.commandLine.appendSwitch('in-process-gpu');

function debug(message: string): void {
  try {
    fsSync.mkdirSync(path.dirname(debugLog), {recursive: true});
    fsSync.appendFileSync(debugLog, `${new Date().toISOString()} ${message}\n`);
  } catch {
    /* ignore */
  }
}

debug(`argv=${JSON.stringify(process.argv)}`);
debug(`flags=${JSON.stringify(flags)}`);
debug(`dirname=${__dirname}`);

// --user-data must be applied BEFORE anything that reads userData (settings,
// single-instance lock): the lock is keyed on the userData path, so a dev
// instance running alongside the installed app needs a different userData to
// get its own lock.
if (typeof flags['user-data'] === 'string' && flags['user-data']) {
  app.setPath('userData', path.resolve(flags['user-data']));
}

// ── Elevation + app-level settings (userData/settings.json) ─────────────────
// Both must be settled before the window exists; the elevate preference is
// read main-side because the relaunch decision happens before the renderer
// boots (unlike color scheme, which only the renderer needs).

// Synchronous probe: the elevate-on-start decision below needs the answer now.
let elevated = false;
if (process.platform === 'win32') {
  try {
    execSync('net session', {stdio: 'ignore'});
    elevated = true;
  } catch {
    elevated = false;
  }
  debug(`elevated=${elevated}`);
}

type AppSettings = {elevate?: boolean};

function readAppSettings(): AppSettings {
  try {
    return JSON.parse(fsSync.readFileSync(path.join(app.getPath('userData'), 'settings.json'), 'utf8')) as AppSettings;
  } catch {
    return {};
  }
}

function writeAppSettings(patch: AppSettings): void {
  try {
    fsSync.mkdirSync(app.getPath('userData'), {recursive: true});
    const next = {...readAppSettings(), ...patch};
    fsSync.writeFileSync(path.join(app.getPath('userData'), 'settings.json'), JSON.stringify(next, null, 2));
  } catch {
    /* ignore */
  }
}

const appSettings = readAppSettings();

let elevateRelaunchAttempted = false;
/**
 * Re-launch this app elevated (PowerShell Start-Process -Verb RunAs — one UAC
 * prompt). Returns false when already attempted / not applicable. On UAC
 * accept the new instance starts and this one exits; on decline we simply
 * stay non-elevated and never retry (no loops).
 */
function relaunchElevated(): boolean {
  if (elevateRelaunchAttempted || process.platform !== 'win32') return false;
  elevateRelaunchAttempted = true;
  const psPath = process.execPath.replace(/'/g, "''");
  // The child polls for the single-instance lock (--elevate-relaunch) because
  // this instance is still holding it when the child boots.
  const argList = process.argv
    .slice(1)
    .map((arg) => (/[\s"]/.test(arg) ? `"${arg}"` : arg))
    .filter((arg) => arg !== '--elevate-relaunch')
    .concat('--elevate-relaunch')
    .join(' ');
  const cmd =
    `Start-Process -FilePath '${psPath}' -Verb RunAs` +
    (argList ? ` -ArgumentList '${argList.replace(/'/g, "''")}'` : '');
  const child = spawn('powershell.exe', ['-NoProfile', '-Command', cmd], {
    stdio: 'ignore',
    windowsHide: true,
  });
  child.on('exit', (code) => {
    debug(`relaunchElevated exit=${code}`);
    if (code === 0) {
      // Short grace so this instance's own debug writes flush; the relaunched
      // instance polls for the single-instance lock in case it boots first.
      setTimeout(() => app.exit(0), 400);
    }
    // non-zero = UAC declined or launch failed: stay non-elevated
  });
  child.on('error', () => debug('relaunchElevated spawn error'));
  return true;
}

// Launch preference: restart elevated once at startup when enabled.
if (appSettings.elevate && !elevated) {
  relaunchElevated();
}

// One instance owns the RAM disk and the CLI; a second launch (e.g. double
// clicking a .dcp) forwards the path to the running window instead.
//
// An elevated relaunch (`--elevate-relaunch`, set by relaunchElevated) can
// reach this point while the pre-elevation instance is still alive — it only
// exits ~400ms after spawning us. Poll for the lock instead of quitting, or
// every UAC handoff would silently kill the fresh instance.
const elevateRelaunch = process.argv.includes('--elevate-relaunch');
let instanceLock = app.requestSingleInstanceLock();
if (!instanceLock && elevateRelaunch) {
  for (let attempt = 0; attempt < 40 && !instanceLock; attempt++) {
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 100);
    instanceLock = app.requestSingleInstanceLock();
  }
}
if (!instanceLock) {
  app.quit();
} else {
  app.on('second-instance', (_event, argv) => {
    const win = BrowserWindow.getAllWindows()[0];
    if (!win) return;
    if (win.isMinimized()) win.restore();
    win.focus();
    const dcp = [...argv].reverse().find((item) => /\.dcp$/i.test(item));
    if (dcp && fsSync.existsSync(dcp)) {
      win.webContents.send('pipeline:event', {type: 'open_dcp', path: dcp});
    }
  });
}

function createWindow(): BrowserWindow {
  const offscreen = Boolean(flags.screenshot);
  const win = new BrowserWindow({
    width: 1400,
    height: 900,
    minWidth: 960,
    minHeight: 640,
    show: !offscreen,
    frame: false,
    backgroundColor: '#0b0b0b',
    autoHideMenuBar: true,
    // Packaged windows take the exe's icon; dev needs it spelled out.
    icon: app.isPackaged ? undefined : path.join(app.getAppPath(), 'assets', 'icon.png'),
    paintWhenInitiallyHidden: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
      offscreen,
      backgroundThrottling: false,
    },
  });
  win.setMenuBarVisibility(false);

  const rendererUrl = process.env.ELECTRON_RENDERER_URL;
  // External links (settings tips, future help links) open in the system
  // browser — never navigate the app window.
  const appOrigin = rendererUrl ? new URL(rendererUrl).origin : '';
  win.webContents.setWindowOpenHandler(({url}) => {
    if (/^https?:\/\//i.test(url)) void shell.openExternal(url);
    return {action: 'deny'};
  });
  win.webContents.on('will-navigate', (event, url) => {
    if (/^https?:\/\//i.test(url) && url !== appOrigin) {
      event.preventDefault();
      void shell.openExternal(url);
    }
  });
  if (rendererUrl) {
    void win.loadURL(rendererUrl);
  } else {
    void win.loadFile(path.join(__dirname, '../dist/index.html'));
  }
  return win;
}

function ipcExportRequest(raw: unknown): ExportRequest {
  const body = (raw ?? {}) as ExportRequest;
  return body;
}

app.whenReady().then(() => {
  const win = createWindow();
  win.once('ready-to-show', () => win.show());

  ipcMain.handle('pipeline:probe', () => orchestrator.probe());
  ipcMain.handle('pipeline:inspect', (_event, blend: unknown) => orchestrator.inspect(String(blend || '')));
  ipcMain.handle('pipeline:start', (_event, raw: unknown) => orchestrator.start(ipcExportRequest(raw)));
  ipcMain.handle('pipeline:stop', () => orchestrator.stop());
  ipcMain.handle('nrtool:open', (_event, params: unknown) =>
    orchestrator.nrOpen((params ?? {}) as {color_source: string; buffer_dir: string})
  );
  ipcMain.handle('nrtool:preview', (_event, frame: unknown, settings: unknown) =>
    orchestrator.nrPreview(Number(frame ?? 0), (settings ?? {}) as Record<string, unknown>)
  );
  ipcMain.handle('nrtool:export', (_event, params: unknown) =>
    orchestrator.nrExport((params ?? {}) as Record<string, unknown>)
  );
  ipcMain.handle('nrtool:close', () => orchestrator.nrClose());
  ipcMain.handle('nrtool:stop', () => orchestrator.nrStop());
  ipcMain.handle('dialog:nrVideo', async () => {
    const result = await dialog.showOpenDialog(win, {
      title: '选择色彩源（视频或 PNG16 序列首帧）',
      filters: [
        {name: '色彩源（视频 / PNG）', extensions: ['mov', 'mp4', 'm4v', 'mkv', 'avi', 'png']},
        {name: '视频', extensions: ['mov', 'mp4', 'm4v', 'mkv', 'avi']},
        {name: 'PNG 图片', extensions: ['png']},
        {name: '所有文件', extensions: ['*']},
      ],
      properties: ['openFile'],
    });
    return result.canceled ? null : result.filePaths[0] ?? null;
  });
  ipcMain.handle('dialog:pickDir', async (_event, title: unknown) => {
    const result = await dialog.showOpenDialog(win, {
      title: String(title || '选择目录'),
      properties: ['openDirectory', 'createDirectory'],
    });
    return result.canceled ? null : result.filePaths[0] ?? null;
  });
  ipcMain.handle('dialog:nrVideoSave', async (_event, defaultPath: unknown) => {
    const result = await dialog.showSaveDialog(win, {
      title: '保存 NR 视频',
      defaultPath: String(defaultPath || ''),
      filters: [
        {name: 'QuickTime', extensions: ['mov']},
        {name: 'MP4', extensions: ['mp4']},
      ],
    });
    return result.canceled || !result.filePath ? null : result.filePath;
  });
  ipcMain.handle('dialog:blend', async () => {
    const result = await dialog.showOpenDialog(win, {
      title: '打开 Blender 文件',
      filters: [{name: 'Blender', extensions: ['blend']}],
      properties: ['openFile'],
    });
    return result.canceled ? null : result.filePaths[0] ?? null;
  });
  ipcMain.handle('dialog:exportDir', async () => {
    const result = await dialog.showOpenDialog(win, {
      title: '选择导出目录',
      properties: ['openDirectory', 'createDirectory'],
    });
    return result.canceled ? null : result.filePaths[0] ?? null;
  });
  ipcMain.handle('dialog:dcpOpen', async () => {
    const result = await dialog.showOpenDialog(win, {
      title: '打开工程',
      filters: [{name: 'DLSS Compositor', extensions: ['dcp']}],
      properties: ['openFile'],
    });
    return result.canceled ? null : result.filePaths[0] ?? null;
  });
  ipcMain.handle('dialog:dcpSave', async (_event, defaultPath: unknown) => {
    const result = await dialog.showSaveDialog(win, {
      title: '保存工程',
      defaultPath: String(defaultPath || ''),
      filters: [{name: 'DLSS Compositor', extensions: ['dcp']}],
    });
    if (result.canceled || !result.filePath) return null;
    return ensureDcpExtension(result.filePath);
  });
  ipcMain.handle('project:load', (_event, filePath: unknown) => loadDcpFile(String(filePath || '')));
  ipcMain.handle('project:save', (_event, filePath: unknown, project: unknown) =>
    saveDcpFile(String(filePath || ''), project as DcpProject)
  );
  ipcMain.handle('project:recent', () => listRecent());
  ipcMain.handle('project:remember', (_event, filePath: unknown) => rememberRecent(String(filePath || '')));
  ipcMain.handle('fs:exists', (_event, filePath: unknown) => fileExists(String(filePath || '')));
  ipcMain.handle('shell:openPath', async (_event, filePath: unknown) => {
    const target = String(filePath || '');
    if (!target) return false;
    const error = await shell.openPath(target);
    return !error;
  });
  ipcMain.handle('fs:listDir', async (_event, dirPath: unknown) => {
    const target = String(dirPath || '');
    if (!target) return null;
    try {
      const dirents = await fs.readdir(target, {withFileTypes: true});
      const entries = await Promise.all(
        dirents.map(async (dirent) => {
          let size = 0;
          let mtime = 0;
          if (!dirent.isDirectory()) {
            try {
              const stat = await fs.stat(path.join(target, dirent.name));
              size = stat.size;
              mtime = stat.mtimeMs;
            } catch {
              /* raced delete */
            }
          }
          return {name: dirent.name, isDir: dirent.isDirectory(), size, mtime};
        })
      );
      return entries;
    } catch {
      return null;
    }
  });
  ipcMain.handle('app:launchFlags', () => ({
    blend: typeof flags.blend === 'string' ? flags.blend : null,
    dcp: typeof flags.dcp === 'string' ? flags.dcp : null,
    home: Boolean(flags.home),
    autoExport: Boolean(flags['auto-export']),
    page: typeof flags.page === 'string' ? flags.page : null,
    rail: typeof flags.rail === 'string' ? flags.rail : null,
    elevated,
    packaged,
    elevatePref: Boolean(appSettings.elevate),
    version: app.getVersion(),
  }));
  ipcMain.handle('app:setCliLog', (_event, enabled: unknown) => {
    orchestrator.setCliLog(Boolean(enabled));
  });
  ipcMain.handle('app:setElevate', (_event, enabled: unknown) => {
    const next = Boolean(enabled);
    writeAppSettings({elevate: next});
    // Turning it on in a non-elevated session elevates right away; turning it
    // off just affects the next launch (this session stays elevated).
    if (next && !elevated) relaunchElevated();
  });
  ipcMain.handle('window:setTitle', (_event, title: unknown) => {
    win.setTitle(String(title || 'DLSS Compositor'));
  });
  ipcMain.handle('window:minimize', () => win.minimize());
  ipcMain.handle('window:maximize', () => {
    if (win.isMaximized()) win.unmaximize();
    else win.maximize();
  });
  ipcMain.handle('window:close', () => win.close());
  ipcMain.handle('window:isMaximized', () => win.isMaximized());
  win.on('maximize', () => win.webContents.send('window:maximized', true));
  win.on('unmaximize', () => win.webContents.send('window:maximized', false));
  ipcMain.handle('preview:dataUrl', async (_event, filePath: string) => {
    if (!filePath) return null;
    try {
      const buf = await fs.readFile(filePath);
      return `data:image/png;base64,${buf.toString('base64')}`;
    } catch {
      return null;
    }
  });

  orchestrator.onEvent((event) => {
    if (!win.isDestroyed()) {
      win.webContents.send('pipeline:event', event);
    }
  });

  const autoExport = Boolean(flags['auto-export']);
  const flagBlend = typeof flags.blend === 'string' ? flags.blend : '';
  if (flagBlend) {
    win.webContents.once('did-finish-load', () => {
      void (async () => {
        win.webContents.send('pipeline:event', {type: 'blend_selected', blend: flagBlend});
        const inspected = await orchestrator.inspect(flagBlend);
        if (!inspected.ok) {
          win.webContents.send('pipeline:event', {type: 'inspect_error', error: inspected.error});
          if (!autoExport) return;
        }
        if (!autoExport) return;
        const overrides: SceneOverrides = {};
        if (flags.percentage) overrides.percentage = Number(flags.percentage);
        if (flags['frame-start']) overrides.frameStart = Number(flags['frame-start']);
        if (flags['frame-end']) overrides.frameEnd = Number(flags['frame-end']);
        if (flags.samples) overrides.samples = Number(flags.samples);
        if (flags['use-adaptive-sampling'] != null) {
          overrides.useAdaptiveSampling = String(flags['use-adaptive-sampling']) === 'true';
        }
        if (flags['adaptive-threshold']) overrides.adaptiveThreshold = Number(flags['adaptive-threshold']);
        if (flags['adaptive-min-samples']) overrides.adaptiveMinSamples = Number(flags['adaptive-min-samples']);
        if (flags['time-limit'] != null && flags['time-limit'] !== true) {
          overrides.timeLimit = Number(flags['time-limit']);
        }
        if (flags['use-denoising'] != null) {
          overrides.useDenoising = String(flags['use-denoising']) === 'true';
        }
        if (flags.denoiser) overrides.denoiser = String(flags.denoiser);
        const scene = inspected.scene;
        const outputFormat = String(flags['output-format'] || 'prores_422hq');
        const resolved = resolveFilenameTemplate(
          typeof flags.template === 'string' ? flags.template : DEFAULT_FILENAME_TEMPLATE,
          {
            job: jobTokenFromBlend(flagBlend),
            mode: String(flags.mode || 'sr'),
            version: flags.version ? Number(flags.version) : 1,
            format: outputFormat,
          }
        );
        if (!resolved.ok) {
          win.webContents.send('pipeline:event', {type: 'error', message: resolved.error});
          return;
        }
        const req: ExportRequest = {
          blend: flagBlend,
          mode: String(flags.mode || 'sr'),
          quality: String(flags.quality || 'BALANCED'),
          frameStart: overrides.frameStart ?? scene?.frame_start ?? 1,
          frameEnd: overrides.frameEnd ?? scene?.frame_end ?? 10,
          outputWidth: flags['output-width'] ? Number(flags['output-width']) : scene?.resolution_x,
          outputHeight: flags['output-height'] ? Number(flags['output-height']) : scene?.resolution_y,
          viewTransform: flags['view-transform'] ? String(flags['view-transform']) : undefined,
          displayDevice: flags['display-device'] ? String(flags['display-device']) : undefined,
          outputFormat,
          exportDir: String(flags['export-dir'] || path.join(path.resolve(__dirname, '../..'), 'build', 'stage4-out')),
          outputTemplate: resolved.template,
          overrides,
          nativeResBuffers: Boolean(flags['native-res-buffers']),
          nrBuffer: Boolean(flags['nr-buffer']),
          fg: parseFg(flags.fg),
          ramWindowFrames: flags['ram-window-frames'] ? Number(flags['ram-window-frames']) : undefined,
          useRamdisk: flags['cache-path'] ? false : true,
          cachePath: flags['cache-path'] ? String(flags['cache-path']) : '',
          effectivePercentage: flags.percentage ? Number(flags.percentage) : undefined,
          window: flags.window ? Number(flags.window) : undefined,
        };
        debug(`auto-export req=${JSON.stringify(req)}`);
        win.webContents.send('pipeline:event', {type: 'auto_export', ...req});
        const result = await orchestrator.start(req);
        debug(`auto-export result=${JSON.stringify(result)}`);
        if (!result.ok) {
          win.webContents.send('pipeline:event', {type: 'error', message: result.error});
        }
      })();
    });
  }

  const shotPath = typeof flags.screenshot === 'string' ? flags.screenshot : '';
  const flagDcp = typeof flags.dcp === 'string' ? flags.dcp : '';
  const wantHome = Boolean(flags.home) || (!flagBlend && !flagDcp && !autoExport);
  if (shotPath) {
    let captured = false;
    const capture = async () => {
      if (captured || win.isDestroyed()) return;
      captured = true;
      await win.webContents.setZoomFactor(1);
      let png = (await win.webContents.capturePage()).toPNG();
      if (png.length < 100) {
        try {
          if (!win.webContents.debugger.isAttached()) {
            win.webContents.debugger.attach('1.3');
          }
          const result = (await win.webContents.debugger.sendCommand('Page.captureScreenshot', {
            format: 'png',
            captureBeyondViewport: true,
          })) as {data?: string};
          if (result.data) {
            png = Buffer.from(result.data, 'base64');
          }
        } catch (err) {
          debug(`debugger screenshot failed: ${err instanceof Error ? err.message : String(err)}`);
        }
      }
      debug(`screenshot bytes=${png.length} path=${shotPath}`);
      await fs.mkdir(path.dirname(shotPath), {recursive: true});
      await fs.writeFile(shotPath, png);
    };
    const finishShot = async (delayMs: number) => {
      await new Promise((r) => setTimeout(r, delayMs));
      await capture();
      // `--dump-style=<selector>`: computed-style probe for the harness —
      // the F12 panel for offscreen runs. Writes <shot>.styles.json with
      // geometry + a fixed style whitelist (including ::before, which the
      // rail indicators are drawn with).
      const dumpSelector = typeof flags['dump-style'] === 'string' ? flags['dump-style'] : '';
      if (dumpSelector && !win.isDestroyed()) {
        const dump = await win.webContents.executeJavaScript(
          `(() => {
            const PROPS = ['display','position','width','height','boxSizing','padding','margin','marginInlineEnd','marginInlineStart','borderRadius','backgroundColor','backgroundImage','alignItems','justifyContent','gap','opacity','color','overflow','insetInlineStart','insetInlineEnd','flexBasis','flexShrink','flex','minWidth','transition'];
            const pick = (cs) => Object.fromEntries(PROPS.map((p) => [p, String(cs[p] ?? '')]));
            return JSON.stringify([...document.querySelectorAll(${JSON.stringify(dumpSelector)})].map((el) => {
              const r = el.getBoundingClientRect();
              return {
                tag: el.tagName.toLowerCase(),
                cls: el.getAttribute('class'),
                slot: el.getAttribute('slot'),
                aria: el.getAttribute('aria-label'),
                rect: {x: r.x, y: r.y, w: r.width, h: r.height},
                style: pick(getComputedStyle(el)),
                before: (() => { const p = getComputedStyle(el, '::before'); return {content: p.content, width: p.width, height: p.height, opacity: p.opacity, background: p.backgroundColor, insetInlineStart: p.insetInlineStart, top: p.top}; })(),
              };
            }));
          })()`,
        );
        await fs.writeFile(shotPath.replace(/\.png$/i, '') + '.styles.json', dump);
      }
      if (flags['quit-on-shot']) {
        app.quit();
      }
    };
    if (wantHome) {
      win.webContents.once('did-finish-load', () => {
        void finishShot(1800);
      });
    } else {
      orchestrator.onEvent((event) => {
        debug(`event=${event.type} ${JSON.stringify(event).slice(0, 400)}`);
        const inspectSettled = event.type === 'inspect_done' || event.type === 'inspect_error';
        const exportSettled = event.type === 'export_done' || event.type === 'export_error' || event.type === 'error';
        if (flags['demo-progress'] && inspectSettled) {
          void (async () => {
            await new Promise((r) => setTimeout(r, 1800));
            if (win.isDestroyed()) return;
            win.webContents.send('pipeline:event', {type: 'export_start', mode: 'rr'});
            await new Promise((r) => setTimeout(r, 2100));
            if (!win.isDestroyed()) {
              win.webContents.send('pipeline:event', {type: 'engine_progress', current_frame: 2});
            }
            await finishShot(800);
          })();
          return;
        }
        if (flags['demo-error'] && inspectSettled) {
          void (async () => {
            await new Promise((r) => setTimeout(r, 1200));
            if (win.isDestroyed()) return;
            win.webContents.send('pipeline:event', {type: 'export_start', mode: 'sr'});
            await new Promise((r) => setTimeout(r, 1600));
            if (!win.isDestroyed()) {
              win.webContents.send('pipeline:event', {type: 'engine_progress', current_frame: 2});
            }
            await new Promise((r) => setTimeout(r, 400));
            if (!win.isDestroyed()) {
              win.webContents.send('pipeline:event', {
                type: 'error',
                message:
                  '{"event": "error", "message": "drive R: is not mounted; Stage 3 will not create a RAM disk", "missing": false} drive R: is not mounted; Stage 3 will not create a RAM disk',
              });
            }
            await finishShot(900);
          })();
          return;
        }
        if (flags['demo-phase'] && inspectSettled) {
          void (async () => {
            await new Promise((r) => setTimeout(r, 1200));
            if (win.isDestroyed()) return;
            win.webContents.send('pipeline:event', {type: 'export_start', mode: 'sr'});
            await new Promise((r) => setTimeout(r, 600));
            if (!win.isDestroyed()) {
              win.webContents.send('pipeline:event', {
                type: 'job_phase',
                message: '引导 0830–0835',
                phase: 'guides',
                frame_start: 830,
                frame_end: 835,
              });
            }
            await finishShot(900);
          })();
          return;
        }
        if (flags['demo-rr'] && inspectSettled) {
          void (async () => {
            await new Promise((r) => setTimeout(r, 1200));
            if (win.isDestroyed()) return;
            win.webContents.send('pipeline:event', {
              type: 'ui_state',
              mode: 'rr',
              nativeResBuffers: true,
            });
            await finishShot(900);
          })();
          return;
        }
        if (flags['demo-rr-fg'] && inspectSettled) {
          void (async () => {
            await new Promise((r) => setTimeout(r, 1200));
            if (win.isDestroyed()) return;
            win.webContents.send('pipeline:event', {
              type: 'ui_state',
              mode: 'rr',
              fg: 2,
              nativeResBuffers: false,
            });
            await finishShot(900);
          })();
          return;
        }
        if (flags['demo-rr-fg-run'] && inspectSettled) {
          void (async () => {
            await new Promise((r) => setTimeout(r, 1200));
            if (win.isDestroyed()) return;
            win.webContents.send('pipeline:event', {
              type: 'ui_state',
              mode: 'rr',
              fg: 2,
              nativeResBuffers: false,
            });
            await new Promise((r) => setTimeout(r, 400));
            if (win.isDestroyed()) return;
            win.webContents.send('pipeline:event', {
              type: 'auto_export',
              mode: 'rr',
              fg: 2,
              frameStart: 2000,
              frameEnd: 2003,
              blend: flagBlend,
            });
            await new Promise((r) => setTimeout(r, 400));
            if (!win.isDestroyed()) {
              win.webContents.send('pipeline:event', {
                type: 'export_start',
                mode: 'rr',
                fg: 2,
              });
            }
            await finishShot(900);
          })();
          return;
        }
        if (flags['demo-fg'] && inspectSettled) {
          void (async () => {
            await new Promise((r) => setTimeout(r, 1200));
            if (win.isDestroyed()) return;
            win.webContents.send('pipeline:event', {
              type: 'ui_state',
              mode: 'sr',
              fg: 2,
            });
            await new Promise((r) => setTimeout(r, 400));
            if (win.isDestroyed()) return;
            win.webContents.send('pipeline:event', {
              type: 'auto_export',
              mode: 'sr',
              fg: 2,
              frameStart: 2000,
              frameEnd: 2003,
              blend: flagBlend,
            });
            await new Promise((r) => setTimeout(r, 600));
            if (!win.isDestroyed()) {
              win.webContents.send('pipeline:event', {
                type: 'job_phase',
                message: '渲染 2000–2003',
                phase: 'combined',
                frame_start: 2000,
                frame_end: 2003,
              });
            }
            await finishShot(900);
          })();
          return;
        }
        const shotOn = typeof flags['shot-on'] === 'string' ? flags['shot-on'] : '';
        const shotDelay = flags['shot-delay'] ? Number(flags['shot-delay']) : 800;
        if (shotOn && event.type === shotOn) {
          void finishShot(Number.isFinite(shotDelay) && shotDelay > 0 ? shotDelay : 800);
          return;
        }
        const shouldShot = autoExport ? exportSettled : inspectSettled;
        if (shouldShot) {
          void finishShot(1500);
        }
      });
    }
  }

  // Memory harness: spam synthetic full-resolution preview frames and log per-process
  // metrics, so renderer-side growth is measurable without a real export.
  // `--demo-preview [--demo-seconds N]` — requires build/demo-preview.jpg.
  if (flags['demo-preview']) {
    const demoSeconds = Number(flags['demo-seconds']) > 0 ? Number(flags['demo-seconds']) : 60;
    win.webContents.once('did-finish-load', () => {
      void (async () => {
        const jpegPath = path.resolve(__dirname, '../../build/demo-preview.jpg');
        let b64 = '';
        try {
          b64 = (await fs.readFile(jpegPath)).toString('base64');
        } catch {
          debug('demo-preview: build/demo-preview.jpg missing');
          app.quit();
          return;
        }
        debug(`demo-preview start b64=${b64.length}B seconds=${demoSeconds}`);
        const sendTimer = setInterval(() => {
          if (!win.isDestroyed()) {
            win.webContents.send('pipeline:event', {
              type: 'preview_frame',
              jpeg_b64: b64,
              width: 3840,
              height: 2160
            });
          }
        }, 200);
        const memTimer = setInterval(() => {
          const metrics = app
            .getAppMetrics()
            .map((m) => `${m.type}#${m.pid}=${m.memory.workingSetSize}KB`)
            .join(' ');
          debug(`mem rss=${Math.round(process.memoryUsage().rss / 1048576)}MB ${metrics}`);
        }, 3000);
        setTimeout(() => {
          clearInterval(sendTimer);
          clearInterval(memTimer);
          debug('demo-preview done');
          app.quit();
        }, demoSeconds * 1000);
      })();
    });
  }
});

app.on('window-all-closed', () => {
  app.quit();
});

// Quit must WAIT for orchestrator teardown: stop jobs/CLI, then unmount the
// RAM disk this process auto-created. `void shutdown(); app.quit()` raced the
// async imdisk detach against process exit, so the disk stayed mounted.
let quitTeardownStarted = false;
app.on('before-quit', (event) => {
  if (quitTeardownStarted) return;  // our own app.exit() path — let it through
  quitTeardownStarted = true;
  event.preventDefault();
  // Never wedge the app on a hung RPC/imdisk call.
  const force = setTimeout(() => app.exit(0), 10000);
  void orchestrator.shutdown().finally(() => {
    clearTimeout(force);
    app.exit(0);
  });
});
