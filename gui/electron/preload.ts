import { contextBridge, ipcRenderer } from 'electron'
import { IPC_CHANNELS } from './ipc-channels'

contextBridge.exposeInMainWorld('dlssApi', {
  selectDirectory: (): Promise<string> =>
    ipcRenderer.invoke('dialog:openDirectory'),
  selectFile: (filters: Electron.FileFilter[]): Promise<string> =>
    ipcRenderer.invoke('dialog:openFile', filters),
  startProcessing: (config: unknown): void =>
    ipcRenderer.send('process:start', { config }),
  stopProcessing: (): void =>
    ipcRenderer.send('process:stop'),
  onProgress: (callback: (data: { current: number; total: number }) => void): (() => void) => {
    const handler = (_event: Electron.IpcRendererEvent, data: { current: number; total: number }) => callback(data)
    ipcRenderer.on('process:progress', handler)
    return () => ipcRenderer.removeListener('process:progress', handler)
  },
  onError: (callback: (message: string) => void): (() => void) => {
    const handler = (_event: Electron.IpcRendererEvent, message: string) => callback(message)
    ipcRenderer.on('process:error', handler)
    return () => ipcRenderer.removeListener('process:error', handler)
  },
  onComplete: (callback: () => void): (() => void) => {
    const handler = () => callback()
    ipcRenderer.on('process:complete', handler)
    return () => ipcRenderer.removeListener('process:complete', handler)
  },
  onLog: (callback: (line: string) => void): (() => void) => {
    const handler = (_event: Electron.IpcRendererEvent, line: string) => callback(line)
    ipcRenderer.on('process:log', handler)
    return () => ipcRenderer.removeListener('process:log', handler)
  },
  onFrameWritten: (callback: (data: { path: string; n: number }) => void): (() => void) => {
    const handler = (_event: Electron.IpcRendererEvent, data: { path: string; n: number }) => callback(data)
    ipcRenderer.on('process:frame-written', handler)
    return () => ipcRenderer.removeListener('process:frame-written', handler)
  },
  onBanner: (
    callback: (data: { sdk: string; engine: string; sr_available: boolean; fg_available: boolean; max_multi_frame: number }) => void
  ): (() => void) => {
    const handler = (_event: Electron.IpcRendererEvent, data: unknown) =>
      callback(data as { sdk: string; engine: string; sr_available: boolean; fg_available: boolean; max_multi_frame: number })
    ipcRenderer.on('process:banner', handler)
    return () => ipcRenderer.removeListener('process:banner', handler)
  },
  getSettings: (): Promise<unknown> =>
    ipcRenderer.invoke('settings:get'),
  saveSettings: (settings: unknown): Promise<void> =>
    ipcRenderer.invoke('settings:save', settings),
  getResolvedExePath: (): Promise<string | null> =>
    ipcRenderer.invoke('exe:get-resolved-path'),
  getLatestFrame: (outputDir: string): Promise<string> =>
    ipcRenderer.invoke('frame:latest', outputDir),
  getFramePreview: (exrPath: string): Promise<string | null> =>
    ipcRenderer.invoke('frame:preview-png', exrPath),
  cameraAutoDetect: (inputDir: string): Promise<{ cameraJsonPath: string | null; sourceWidth: number | null; sourceHeight: number | null }> =>
    ipcRenderer.invoke('camera:auto-detect', inputDir),
  countExrFiles: (dir: string): Promise<number> =>
    ipcRenderer.invoke('count:exr-files', dir),
  readExrHeader: (filePath: string): Promise<{ ok: boolean; data?: unknown; error?: string }> =>
    ipcRenderer.invoke(IPC_CHANNELS.EXR_READ_HEADER, filePath),
})
