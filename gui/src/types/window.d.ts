import type { DlssConfig } from './dlss-config'

interface DlssApi {
  selectDirectory(): Promise<string>
  selectFile(filters: { name: string; extensions: string[] }[]): Promise<string>
  startProcessing(config: DlssConfig, exePath?: string): void
  stopProcessing(): void
  onProgress(callback: (data: { current: number; total: number }) => void): void
  onError(callback: (message: string) => void): void
  onComplete(callback: () => void): void
  onLog(callback: (line: string) => void): void
  getSettings(): Promise<unknown>
  saveSettings(settings: unknown): Promise<void>
  validateExePath(path: string): Promise<boolean>
}

declare global {
  interface Window {
    dlssApi: DlssApi
  }
}

export {}
