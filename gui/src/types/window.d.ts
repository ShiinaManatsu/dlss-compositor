import type { DlssConfig } from './dlss-config'

type DlssApiUnlisten = () => void

interface DlssApi {
  selectDirectory(): Promise<string>
  selectFile(filters: { name: string; extensions: string[] }[]): Promise<string>
  startProcessing(config: DlssConfig, exePath?: string): void
  stopProcessing(): void
  onProgress(callback: (data: { current: number; total: number }) => void): DlssApiUnlisten
  onError(callback: (message: string) => void): DlssApiUnlisten
  onComplete(callback: () => void): DlssApiUnlisten
  onLog(callback: (line: string) => void): DlssApiUnlisten
  getSettings(): Promise<unknown>
  saveSettings(settings: unknown): Promise<void>
  validateExePath(path: string): Promise<boolean>
  getLatestFrame(outputDir: string): Promise<string>
}

declare global {
  interface Window {
    dlssApi: DlssApi
  }
}

export {}
