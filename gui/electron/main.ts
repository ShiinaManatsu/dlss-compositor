import { app, BrowserWindow, dialog, ipcMain, Menu } from 'electron'
import fs from 'node:fs'
import path from 'node:path'
import { spawn, exec } from 'node:child_process'
import { IPC_CHANNELS } from './ipc-channels'
import { parseNdjsonLine, ProgressLineBuffer } from './progress-parser'
import { activeProcess, buildCliArgs, killProcess, startProcess } from './process-manager'
import { getSettings, saveSettings, loadSettings, resetSettings, type PersistedSettings } from './settings-store'
import { findCameraJson, readSourceResolution } from './camera-detector'
import { exrToPng, cleanupPreviewCache } from './preview-generator'
import { getResolvedExePath, locateCliExecutable } from './exe-locator'
import type { DlssConfig } from '../src/types/dlss-config'

type ProcessStartPayload = {
  config: DlssConfig;
}

function toBannerIpcPayload(event: NonNullable<ReturnType<typeof parseNdjsonLine>>): Record<string, unknown> {
  return {
    ...event,
    sdk: event.sdk ?? event.ngx_sdk_version,
  }
}

function formatProbeExit(code: number | null, signal: NodeJS.Signals | null): string {
  if (code !== null) {
    return `code ${code}`
  }
  return `signal ${signal ?? 'unknown'}`
}

let mainWindow: BrowserWindow | null = null
let cliLocatorError: string | null = null

function probeNgxBanner(exePath: string, win: BrowserWindow): void {
  if (!exePath || !fs.existsSync(exePath)) return

  const proc = spawn(exePath, ['--test-ngx', '--progress-format', 'json'], {
    stdio: ['ignore', 'pipe', 'pipe'],
  })

  let bannerReceived = false
  const handleLine = (line: string) => {
    const event = parseNdjsonLine(line)
    if (!event) return

    // CLI emits {"type":"banner",...} after the Plan 1 T4 NDJSON fix.
    // Accept the legacy `event` discriminator too for older CLI binaries.
    if (event.type === 'banner' || event.event === 'banner') {
      bannerReceived = true
      win.webContents.send(IPC_CHANNELS.PROCESS_BANNER, toBannerIpcPayload(event))
    }
  }

  const stdoutBuffer = new ProgressLineBuffer()
  stdoutBuffer.onLine = handleLine
  proc.stdout?.on('data', (chunk: Buffer) => stdoutBuffer.feed(chunk.toString()))

  // Keep stderr parseable for defensive compatibility if banner routing ever
  // changes. Non-JSON SDK log lines are ignored by parseNdjsonLine().
  const stderrBuffer = new ProgressLineBuffer()
  stderrBuffer.onLine = handleLine
  proc.stderr?.on('data', (chunk: Buffer) => stderrBuffer.feed(chunk.toString()))

  proc.once('exit', (code, signal) => {
    stdoutBuffer.flush()
    stderrBuffer.flush()
    if (!bannerReceived) {
      win.webContents.send(
        IPC_CHANNELS.PROCESS_ERROR,
        `DLSS detection failed: process exited (${formatProbeExit(code, signal)}) without emitting a banner event. ` +
          'Ensure NVIDIA RTX drivers and DLSS DLLs are installed.',
      )
    }
  })

  proc.once('error', (err) => {
    win.webContents.send(
      IPC_CHANNELS.PROCESS_ERROR,
      `DLSS detection failed: could not start process: ${err.message}`,
    )
  })
}

function createWindow(): void {
  const savedSettings = getSettings()
  const savedBounds = savedSettings.windowBounds

  const win = new BrowserWindow({
    width: savedBounds?.width ?? 1024,
    height: savedBounds?.height ?? 768,
    x: savedBounds?.x,
    y: savedBounds?.y,
    minWidth: 1024,
    minHeight: 768,
    title: 'DLSS Compositor',
    webPreferences: {
      preload: path.join(__dirname, 'preload.mjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false
    }
  })

  mainWindow = win

  win.on('close', () => {
    saveSettings({ windowBounds: win.getBounds() })
  })

  win.on('closed', () => {
    if (mainWindow === win) {
      mainWindow = null
    }
  })

  if (process.env.ELECTRON_RENDERER_URL) {
    win.loadURL(process.env.ELECTRON_RENDERER_URL)
  } else {
    win.loadFile(path.join(__dirname, '../dist/index.html'))
  }

  win.webContents.once('did-finish-load', () => {
    if (cliLocatorError) {
      win.webContents.send(IPC_CHANNELS.PROCESS_ERROR, `CLI not found: ${cliLocatorError}`)
      return
    }
    try {
      const exePath = locateCliExecutable()
      probeNgxBanner(exePath, win)
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err)
      win.webContents.send(IPC_CHANNELS.PROCESS_ERROR, `CLI not found: ${message}`)
    }
  })
}

function registerIpcHandlers(): void {
  ipcMain.handle(IPC_CHANNELS.DIALOG_OPEN_DIRECTORY, async () => {
    const { filePaths } = await dialog.showOpenDialog({ properties: ['openDirectory'] })

    return filePaths[0] ?? ''
  })

  ipcMain.handle(IPC_CHANNELS.DIALOG_OPEN_FILE, async (_event, filters: Electron.FileFilter[] = []) => {
    const { filePaths } = await dialog.showOpenDialog({
      properties: ['openFile'],
      filters,
    })

    return filePaths[0] ?? ''
  })

  ipcMain.on(IPC_CHANNELS.PROCESS_START, (_event, payload: ProcessStartPayload) => {
    const win = mainWindow

    if (!win) {
      return
    }

    if (activeProcess) {
      killProcess(activeProcess)
    }

    let exePath: string
    try {
      exePath = locateCliExecutable()
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err)
      win.webContents.send(IPC_CHANNELS.PROCESS_ERROR, `CLI not found: ${message}`)
      return
    }

    const config = payload?.config
    if (!config) {
      win.webContents.send(IPC_CHANNELS.PROCESS_ERROR, 'Missing config payload')
      return
    }

    const proc = startProcess(exePath, buildCliArgs(config, exePath))

    const lineBuffer = new ProgressLineBuffer()
    lineBuffer.onLine = (line: string) => {
      console.log('[main] stdout line:', JSON.stringify(line))

      const event = parseNdjsonLine(line)
      if (event) {
        const eventType = event.event ?? event.type
        switch (eventType) {
          case 'started':
            // Initial total announcement — surface as a 0/total progress tick
            win.webContents.send(IPC_CHANNELS.PROCESS_PROGRESS, { current: 0, total: event.total })
            break
          case 'frame_started':
            win.webContents.send(IPC_CHANNELS.PROCESS_PROGRESS, { current: event.n, total: event.total })
            break
          case 'frame_written':
            win.webContents.send(IPC_CHANNELS.PROCESS_FRAME_WRITTEN, { path: event.path, n: event.n })
            break
          case 'banner':
            win.webContents.send(IPC_CHANNELS.PROCESS_BANNER, toBannerIpcPayload(event))
            break
          case 'complete':
            // PROCESS_COMPLETE is sent by the 'exit' handler below; just bump
            // the progress to 100% here so the UI lands on total/total.
            win.webContents.send(IPC_CHANNELS.PROCESS_PROGRESS, { current: event.total, total: event.total })
            break
          case 'error':
            win.webContents.send(IPC_CHANNELS.PROCESS_ERROR, event.message ?? event.detail ?? event.reason ?? 'Unknown CLI error')
            break
        }
      } else {
        // Non-JSON line — treat as plain log
        win.webContents.send(IPC_CHANNELS.PROCESS_LOG, line)
      }
    }

    proc.stdout?.on('data', (chunk: Buffer) => {
      lineBuffer.feed(chunk.toString())
    })

    // Route stderr to log (not error IPC) — the NGX/Vulkan SDK writes verbose
    // telemetry to stderr that is not an error. Only flag errors on bad exit code.
    const stderrBuffer = new ProgressLineBuffer()
    stderrBuffer.onLine = (line: string) => {
      win.webContents.send(IPC_CHANNELS.PROCESS_LOG, `[stderr] ${line}`)
    }
    proc.stderr?.on('data', (chunk) => {
      stderrBuffer.feed(chunk.toString())
    })

    proc.once('error', (error) => {
      win.webContents.send(IPC_CHANNELS.PROCESS_ERROR, error.message)
    })

    proc.once('exit', (code) => {
      lineBuffer.flush()
      stderrBuffer.flush()
      if (code !== 0 && code !== null) {
        win.webContents.send(IPC_CHANNELS.PROCESS_ERROR, `Process exited with code ${code}`)
      }
      win.webContents.send(IPC_CHANNELS.PROCESS_COMPLETE)
    })
  })

  ipcMain.on(IPC_CHANNELS.PROCESS_STOP, () => {
    if (activeProcess) {
      killProcess(activeProcess)
    }
  })

  ipcMain.handle(IPC_CHANNELS.SETTINGS_GET, () => {
    return getSettings()
  })

  ipcMain.handle(IPC_CHANNELS.SETTINGS_SAVE, async (_event, settings: Partial<PersistedSettings>) => {
    saveSettings(settings)
  })

  ipcMain.handle(IPC_CHANNELS.SETTINGS_RESET, async () => {
    await resetSettings()
    return getSettings()
  })

  ipcMain.handle(IPC_CHANNELS.EXE_GET_RESOLVED_PATH, () => {
    return getResolvedExePath()
  })

  ipcMain.handle(IPC_CHANNELS.CAMERA_AUTO_DETECT, async (_event, inputDir: string) => {
    const cameraJsonPath = findCameraJson(inputDir);
    const resolution = cameraJsonPath ? readSourceResolution(cameraJsonPath) : null;
    return {
      cameraJsonPath,
      sourceWidth: resolution?.width ?? null,
      sourceHeight: resolution?.height ?? null,
    };
  })

  ipcMain.handle(IPC_CHANNELS.COUNT_EXR_FILES, async (_event, dir: string) => {
    try {
      if (!dir || !fs.existsSync(dir)) return 0;
      const files = fs.readdirSync(dir);
      return files.filter(f => f.toLowerCase().endsWith('.exr')).length;
    } catch {
      return 0;
    }
  });

  ipcMain.handle('frame:latest', (_event, outputDir: string): string => {
    try {
      if (!outputDir || !fs.existsSync(outputDir)) return ''
      const files = fs.readdirSync(outputDir)
        .filter(f => f.toLowerCase().endsWith('.exr'))
        .sort()  // alphabetical = chronological for zero-padded frame names
      if (files.length === 0) return ''
      return path.join(outputDir, files[files.length - 1])
    } catch {
      return ''
    }
  })

  ipcMain.handle(IPC_CHANNELS.FRAME_PREVIEW_PNG, async (_event, exrPath: string) => {
    try {
      return await exrToPng(exrPath)
    } catch {
      return null
    }
  })

  ipcMain.handle(IPC_CHANNELS.EXR_READ_HEADER, async (_event, filePath: string): Promise<{ ok: boolean; data?: unknown; error?: string }> => {
    let exePath: string
    try {
      exePath = locateCliExecutable()
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err)
      return { ok: false, error: `CLI not found: ${message}` }
    }

    return new Promise((resolve) => {
      const safeFilePath = filePath.replace(/"/g, '\\"')
      exec(`"${exePath}" --inspect-exr "${safeFilePath}"`, { timeout: 5000 }, (err, stdout, stderr) => {
        if (err) {
          resolve({ ok: false, error: stderr || err.message })
          return
        }
        try {
          resolve({ ok: true, data: JSON.parse(stdout.trim()) })
        } catch {
          resolve({ ok: false, error: `Failed to parse inspector output: ${stdout}` })
        }
      })
    })
  })
}

app.whenReady().then(() => {
  Menu.setApplicationMenu(null)

  loadSettings()

  try {
    locateCliExecutable() // caches the result inside exe-locator
  } catch (err) {
    cliLocatorError = err instanceof Error ? err.message : String(err)
  }

  registerIpcHandlers()
  createWindow()

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      createWindow()
    }
  })
})

app.on('before-quit', () => {
  if (activeProcess) {
    killProcess(activeProcess)
  }
  cleanupPreviewCache()
})

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit()
  }
})
