import Store from 'electron-store'
import type { Settings } from '../src/types/dlss-config'
import fs from 'node:fs'

interface StoreSchema {
  exePath: string
  lastInputDir: string
  lastOutputDir: string
  windowBounds: { x: number; y: number; width: number; height: number } | undefined
  scaleFactor: number
  quality: string
  interpolateFactor: number
  exrCompression: string
  exrDwaQuality: number
  memoryBudgetGB: number
  tonemapMode: string
  inverseTonemapEnabled: boolean
}

const store = new Store<StoreSchema>({
  name: 'dlss-compositor-settings',
  defaults: {
    exePath: '',
    lastInputDir: '',
    lastOutputDir: '',
    windowBounds: undefined,
    scaleFactor: 2.0,
    quality: 'MaxQuality',
    interpolateFactor: 0,
    exrCompression: 'dwaa',
    exrDwaQuality: 95.0,
    memoryBudgetGB: 8,
    tonemapMode: 'pq',
    inverseTonemapEnabled: true,
  },
})

export function getSettings(): Settings {
  return {
    exePath: store.get('exePath'),
    lastInputDir: store.get('lastInputDir'),
    lastOutputDir: store.get('lastOutputDir'),
    windowBounds: store.get('windowBounds'),
    config: {
      scaleFactor: store.get('scaleFactor'),
      quality: store.get('quality') as Settings['config']['quality'],
      interpolateFactor: store.get('interpolateFactor') as 0 | 2 | 4,
      exrCompression: store.get('exrCompression') as Settings['config']['exrCompression'],
      exrDwaQuality: store.get('exrDwaQuality'),
      memoryBudgetGB: store.get('memoryBudgetGB'),
      tonemapMode: store.get('tonemapMode') as 'none' | 'pq',
      inverseTonemapEnabled: store.get('inverseTonemapEnabled'),
    },
  }
}

export function saveSettings(settings: Partial<Settings>): void {
  if (settings.exePath !== undefined) store.set('exePath', settings.exePath)
  if (settings.lastInputDir !== undefined) store.set('lastInputDir', settings.lastInputDir)
  if (settings.lastOutputDir !== undefined) store.set('lastOutputDir', settings.lastOutputDir)
  if (settings.windowBounds !== undefined) store.set('windowBounds', settings.windowBounds)
  if (settings.config) {
    const c = settings.config
    if (c.scaleFactor !== undefined) store.set('scaleFactor', c.scaleFactor)
    if (c.quality !== undefined) store.set('quality', c.quality)
    if (c.interpolateFactor !== undefined) store.set('interpolateFactor', c.interpolateFactor)
    if (c.exrCompression !== undefined) store.set('exrCompression', c.exrCompression)
    if (c.exrDwaQuality !== undefined) store.set('exrDwaQuality', c.exrDwaQuality)
    if (c.memoryBudgetGB !== undefined) store.set('memoryBudgetGB', c.memoryBudgetGB)
    if (c.tonemapMode !== undefined) store.set('tonemapMode', c.tonemapMode)
    if (c.inverseTonemapEnabled !== undefined) store.set('inverseTonemapEnabled', c.inverseTonemapEnabled)
  }
}

export function getExePath(): string {
  return store.get('exePath')
}

export function setExePath(exePath: string): void {
  if (exePath && !exePath.toLowerCase().endsWith('.exe')) {
    throw new Error('Exe path must end with .exe')
  }
  store.set('exePath', exePath)
}

export function validateExePath(exePath: string): boolean {
  if (!exePath) return false
  return fs.existsSync(exePath)
}
