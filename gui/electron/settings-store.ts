import Store from 'electron-store'
import type { Settings } from '../src/types/dlss-config'

interface StoreSchema {
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
  sourceFps: number
  sharpness: number
  ngxLogLevel: string
}

const store = new Store<StoreSchema>({
  name: 'dlss-compositor-settings',
  defaults: {
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
    sourceFps: 30,
    sharpness: 0.0,
    ngxLogLevel: 'warn',
  },
})

export function getSettings(): Settings {
  return {
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
      sourceFps: store.get('sourceFps'),
      sharpness: store.get('sharpness'),
      ngxLogLevel: store.get('ngxLogLevel') as Settings['config']['ngxLogLevel'],
    },
  }
}

export function saveSettings(settings: Partial<Settings>): void {
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
    if (c.sourceFps !== undefined) store.set('sourceFps', c.sourceFps)
    if (c.sharpness !== undefined) store.set('sharpness', c.sharpness)
    if (c.ngxLogLevel !== undefined) store.set('ngxLogLevel', c.ngxLogLevel)
  }
}
