import { app } from 'electron'
import fs from 'node:fs'
import fsp from 'node:fs/promises'
import path from 'node:path'

// Schema version — bump when making incompatible changes
const SCHEMA_VERSION = 1

export interface PersistedSettings {
  schemaVersion: number
  lastInputDir: string | null
  lastOutputDir: string | null
  windowBounds: { x?: number; y?: number; width: number; height: number } | null
  config: {
    scaleFactor: number
    quality: string
    interpolateFactor: number
    exrCompression: string
    exrDwaQuality: number
    memoryBudgetGB: number
    tonemapMode: string
    inverseTonemapEnabled: boolean
    sourceFps: number
    sharpness: number
    ngxLogLevel: string
  }
}

const DEFAULTS: PersistedSettings = {
  schemaVersion: SCHEMA_VERSION,
  lastInputDir: null,
  lastOutputDir: null,
  windowBounds: null,
  config: {
    scaleFactor: 2.0,
    quality: 'MaxQuality',
    interpolateFactor: 0,
    exrCompression: 'dwaa',
    exrDwaQuality: 95.0,
    memoryBudgetGB: 8,
    tonemapMode: 'pq',
    inverseTonemapEnabled: true,
    sourceFps: 30,
    sharpness: 0.0,
    ngxLogLevel: 'warn',
  },
}

let _current: PersistedSettings = JSON.parse(JSON.stringify(DEFAULTS))
let _saveTimer: ReturnType<typeof setTimeout> | null = null

function getSettingsFilePath(): string {
  if (app.isPackaged) {
    return path.join(path.dirname(app.getPath('exe')), 'dlss-compositor.json')
  }
  // Dev: relative to the gui/ directory (process.cwd() when running `npm run dev` from gui/)
  return path.resolve(process.cwd(), 'dlss-compositor.json')
}

function deepMerge(target: PersistedSettings, partial: Partial<PersistedSettings>): PersistedSettings {
  const result: PersistedSettings = {
    schemaVersion: target.schemaVersion,
    lastInputDir: target.lastInputDir,
    lastOutputDir: target.lastOutputDir,
    windowBounds: target.windowBounds,
    config: { ...target.config },
  }
  if (partial.lastInputDir !== undefined) result.lastInputDir = partial.lastInputDir
  if (partial.lastOutputDir !== undefined) result.lastOutputDir = partial.lastOutputDir
  if (partial.windowBounds !== undefined) result.windowBounds = partial.windowBounds
  if (partial.config) {
    result.config = { ...target.config, ...partial.config }
  }
  return result
}

export function loadSettings(): void {
  const filePath = getSettingsFilePath()
  try {
    if (!fs.existsSync(filePath)) return
    const raw = fs.readFileSync(filePath, 'utf-8')
    const parsed = JSON.parse(raw) as Partial<PersistedSettings>
    if (parsed.schemaVersion !== SCHEMA_VERSION) {
      console.warn(
        `[settings] Schema version mismatch (got ${parsed.schemaVersion}, expected ${SCHEMA_VERSION}). Using defaults.`
      )
      return
    }
    _current = deepMerge(DEFAULTS, parsed)
  } catch (err) {
    console.warn('[settings] Failed to load settings, using defaults:', err)
    _current = JSON.parse(JSON.stringify(DEFAULTS))
  }
}

async function writeToDisk(): Promise<void> {
  const filePath = getSettingsFilePath()
  const tmpPath = filePath + '.tmp'
  try {
    await fsp.writeFile(tmpPath, JSON.stringify(_current, null, 2), 'utf-8')
    await fsp.rename(tmpPath, filePath)
  } catch (err) {
    console.error('[settings] Failed to write settings:', err)
    try {
      await fsp.unlink(tmpPath)
    } catch {
      /* ignore */
    }
  }
}

function scheduleSave(): void {
  if (_saveTimer) clearTimeout(_saveTimer)
  _saveTimer = setTimeout(() => {
    _saveTimer = null
    void writeToDisk()
  }, 500)
}

export function getSettings(): PersistedSettings {
  return JSON.parse(JSON.stringify(_current))
}

export function saveSettings(partial: Partial<PersistedSettings>): void {
  _current = deepMerge(_current, partial)
  scheduleSave()
}

export async function resetSettings(): Promise<void> {
  _current = JSON.parse(JSON.stringify(DEFAULTS))
  if (_saveTimer) {
    clearTimeout(_saveTimer)
    _saveTimer = null
  }
  const filePath = getSettingsFilePath()
  try {
    if (fs.existsSync(filePath)) {
      await fsp.unlink(filePath)
    }
  } catch (err) {
    console.error('[settings] Failed to delete settings file:', err)
  }
}
