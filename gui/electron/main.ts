import { app, BrowserWindow, dialog, ipcMain } from 'electron'
import path from 'node:path'
import { IPC_CHANNELS } from './ipc-channels'
import { activeProcess, buildCliArgs, killProcess, startProcess } from './process-manager'
import type { DlssConfig, Settings } from '../src/types/dlss-config'

type ProcessStartPayload = {
  exePath: string;
  config: DlssConfig;
}

const EMPTY_SETTINGS: Settings = {
  exePath: '',
  lastInputDir: '',
  lastOutputDir: '',
  config: {},
}

let mainWindow: BrowserWindow | null = null

function createWindow(): void {
  const win = new BrowserWindow({
    width: 1024,
    height: 720,
    title: 'DLSS Compositor',
    webPreferences: {
      preload: path.join(__dirname, 'preload.mjs'),
      contextIsolation: true,
      nodeIntegration: false
    }
  })

  mainWindow = win

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

    proc.stdout?.on('data', () => {
      win.webContents.send(IPC_CHANNELS.PROCESS_PROGRESS, { current: 0, total: 0 })
    })

    proc.stderr?.on('data', (chunk) => {
      win.webContents.send(IPC_CHANNELS.PROCESS_ERROR, chunk.toString().trim())
    })

    proc.once('error', (error) => {
      win.webContents.send(IPC_CHANNELS.PROCESS_ERROR, error.message)
    })

    proc.once('exit', () => {
      win.webContents.send(IPC_CHANNELS.PROCESS_COMPLETE)
    })
  })

  ipcMain.on(IPC_CHANNELS.PROCESS_STOP, () => {
    if (activeProcess) {
      killProcess(activeProcess)
    }
  })

  ipcMain.handle(IPC_CHANNELS.SETTINGS_GET, () => {
    return EMPTY_SETTINGS
  })

  ipcMain.handle(IPC_CHANNELS.SETTINGS_SAVE, async (_event, _settings: unknown) => {
    return
  })
}

app.whenReady().then(() => {
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
