import { app, BrowserWindow, dialog, ipcMain, Menu } from 'electron'
import fs from 'node:fs'
import path from 'node:path'
import { IPC_CHANNELS } from './ipc-channels'
import { parseNdjsonLine, ProgressLineBuffer } from './progress-parser'
import { activeProcess, buildCliArgs, killProcess, startProcess } from './process-manager'
import { getExePath, getSettings, saveSettings, setExePath, validateExePath } from './settings-store'
import type { DlssConfig, Settings } from '../src/types/dlss-config'

type ProcessStartPayload = {
  exePath: string;
  config: DlssConfig;
}

let mainWindow: BrowserWindow | null = null

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

    const exePath = payload?.exePath ?? ''
    const config = payload?.config

    if (!exePath || !config) {
      win.webContents.send(IPC_CHANNELS.PROCESS_ERROR, 'Missing executable path or config payload')
      return
    }

    const proc = startProcess(exePath, buildCliArgs(config, exePath))

    const lineBuffer = new ProgressLineBuffer()
    lineBuffer.onLine = (line: string) => {
      console.log('[main] stdout line:', JSON.stringify(line))

      const event = parseNdjsonLine(line)
      if (event) {
        switch (event.event) {
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
            win.webContents.send(IPC_CHANNELS.PROCESS_BANNER, event)
            break
          case 'complete':
            // PROCESS_COMPLETE is sent by the 'exit' handler below; just bump
            // the progress to 100% here so the UI lands on total/total.
            win.webContents.send(IPC_CHANNELS.PROCESS_PROGRESS, { current: event.total, total: event.total })
            break
          case 'error':
            win.webContents.send(IPC_CHANNELS.PROCESS_ERROR, event.message)
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

  ipcMain.handle(IPC_CHANNELS.SETTINGS_SAVE, async (_event, settings: Partial<Settings>) => {
    saveSettings(settings)
  })

  ipcMain.handle(IPC_CHANNELS.VALIDATE_EXE_PATH, (_event, exePath: string) => {
    return validateExePath(exePath)
  })

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
}

app.whenReady().then(() => {
  Menu.setApplicationMenu(null)

  const autoExePath = path.join(path.dirname(app.getPath('exe')), 'dlss-compositor.exe')
  const currentExePath = getExePath()
  if (!currentExePath && fs.existsSync(autoExePath) && validateExePath(autoExePath)) {
    setExePath(autoExePath)
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
})

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit()
  }
})
