import { contextBridge, ipcRenderer } from 'electron'

contextBridge.exposeInMainWorld('dlssApi', {
  selectDirectory: (): Promise<string> =>
    ipcRenderer.invoke('dialog:openDirectory'),
  selectFile: (filters: Electron.FileFilter[]): Promise<string> =>
    ipcRenderer.invoke('dialog:openFile', filters),
  startProcessing: (config: unknown, exePath?: string): void =>
    ipcRenderer.send('process:start', { config, exePath: exePath ?? '' }),
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
  getSettings: (): Promise<unknown> =>
    ipcRenderer.invoke('settings:get'),
  saveSettings: (settings: unknown): Promise<void> =>
    ipcRenderer.invoke('settings:save', settings),
  validateExePath: (exePath: string): Promise<boolean> =>
    ipcRenderer.invoke('settings:validate-exe-path', exePath),
  getLatestFrame: (outputDir: string): Promise<string> =>
    ipcRenderer.invoke('frame:latest', outputDir),
})
