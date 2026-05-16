import path from 'node:path'
import fs from 'node:fs'
import { app } from 'electron'

// Resolved once at startup; call locateCliExecutable() to get it.
let _resolvedPath: string | null = null

export function locateCliExecutable(): string {
  if (_resolvedPath) return _resolvedPath

  const isDev = !app.isPackaged

  if (isDev) {
    // Dev mode: main.js lives in dist-electron/, two levels up = project root
    const devPath = path.resolve(__dirname, '../../build/Release/dlss-compositor.exe')
    if (fs.existsSync(devPath)) {
      _resolvedPath = devPath
      return _resolvedPath
    }
  }

  // Packaged: sibling to the GUI exe
  const packagedPath = path.join(path.dirname(app.getPath('exe')), 'dlss-compositor.exe')
  if (fs.existsSync(packagedPath)) {
    _resolvedPath = packagedPath
    return _resolvedPath
  }

  // Last resort: dev path even in packaged mode
  const fallback = path.resolve(__dirname, '../../build/Release/dlss-compositor.exe')
  if (fs.existsSync(fallback)) {
    _resolvedPath = fallback
    return _resolvedPath
  }

  throw new Error(
    `dlss-compositor.exe not found. Searched: ${packagedPath}, dev build. ` +
    `In dev mode, run: cmake --build build --config Release`
  )
}

export function getResolvedExePath(): string | null {
  return _resolvedPath
}
