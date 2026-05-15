import type { DlssConfig } from './dlss-config'

type DlssApiUnlisten = () => void

interface DlssApi {
  selectDirectory(): Promise<string>
  selectFile(filters: { name: string; extensions: string[] }[]): Promise<string>
  startProcessing(config: DlssConfig): void
  stopProcessing(): void
  onProgress(callback: (data: { current: number; total: number }) => void): DlssApiUnlisten
  onError(callback: (message: string) => void): DlssApiUnlisten
  onComplete(callback: () => void): DlssApiUnlisten
  onLog(callback: (line: string) => void): DlssApiUnlisten
  onFrameWritten(callback: (data: { path: string; n: number }) => void): DlssApiUnlisten
  onBanner(
    callback: (data: { sdk: string; engine: string; sr_available: boolean; fg_available: boolean; max_multi_frame: number }) => void
  ): DlssApiUnlisten
  getSettings(): Promise<unknown>
  saveSettings(settings: unknown): Promise<void>
  getResolvedExePath(): Promise<string | null>
  getLatestFrame(outputDir: string): Promise<string>
  getFramePreview(exrPath: string): Promise<string | null>
  cameraAutoDetect(inputDir: string): Promise<{ cameraJsonPath: string | null; sourceWidth: number | null; sourceHeight: number | null }>
  countExrFiles(dir: string): Promise<number>
  readExrHeader(filePath: string): Promise<{ ok: boolean; data?: { width: number; height: number; channels: string[]; compression: string; is_multi_part: boolean }; error?: string }>
}

declare global {
  interface Window {
    dlssApi: DlssApi
  }
}

export {}
