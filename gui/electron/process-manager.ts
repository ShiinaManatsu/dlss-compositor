import { spawn, type ChildProcess } from 'node:child_process'
import { buildCliArgs } from './cli-args'

export { buildCliArgs }

export let activeProcess: ChildProcess | null = null

export function startProcess(exePath: string, args: string[]): ChildProcess {
  const proc = spawn(exePath, args, { stdio: ['ignore', 'pipe', 'pipe'] })

  activeProcess = proc
  proc.once('exit', () => {
    activeProcess = null
  })

  return proc
}

export function killProcess(proc: ChildProcess): void {
  const pid = proc.pid

  if (!proc.killed) {
    proc.kill()
  }

  if (process.platform === 'win32' && pid) {
    const killer = spawn('taskkill', ['/F', '/PID', String(pid)], { stdio: 'ignore', windowsHide: true })

    killer.once('error', () => {
      // Best-effort fallback only.
    })
  }

  if (activeProcess?.pid === pid) {
    activeProcess = null
  }
}
