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
  onProgress: (callback: (data: { current: number; total: number }) => void): void => {
    ipcRenderer.on('process:progress', (_event, data) => callback(data))
  },
  onError: (callback: (message: string) => void): void => {
    ipcRenderer.on('process:error', (_event, message) => callback(message))
  },
  onComplete: (callback: () => void): void => {
    ipcRenderer.on('process:complete', () => callback())
  },
  onLog: (callback: (line: string) => void): void => {
    ipcRenderer.on('process:log', (_event, line) => callback(line))
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
