import {app, BrowserWindow, dialog, ipcMain} from 'electron';
import fs from 'node:fs/promises';
import fsSync from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {PipelineOrchestrator, parseCliFlags, type ExportRequest, type SceneOverrides} from './orchestrator';
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
const orchestrator = new PipelineOrchestrator();
const flags = parseCliFlags(process.argv);
const debugLog = path.resolve(__dirname, '../../build/stage4-out/app-debug.log');
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

if (typeof flags['user-data'] === 'string' && flags['user-data']) {
  app.setPath('userData', path.resolve(flags['user-data']));
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
  ipcMain.handle('app:launchFlags', () => ({
    blend: typeof flags.blend === 'string' ? flags.blend : null,
    dcp: typeof flags.dcp === 'string' ? flags.dcp : null,
    home: Boolean(flags.home),
    autoExport: Boolean(flags['auto-export']),
  }));
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
          fg: parseFg(flags.fg),
          ramWindowFrames: flags['ram-window-frames'] ? Number(flags['ram-window-frames']) : undefined,
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
});

app.on('window-all-closed', () => {
  void orchestrator.shutdown();
  app.quit();
});

app.on('before-quit', () => {
  void orchestrator.shutdown();
});
