import { useState, useEffect, useCallback } from 'react'
import type { DlssConfig } from '../types/dlss-config'
import { setGlobalIsRunning } from '../state/processing-store'

export type ProcessingStatus = 'idle' | 'running' | 'done' | 'error'

export interface ProcessingState {
  status: ProcessingStatus
  progress: number
  currentFrame: number
  totalFrames: number
  errors: string[]
  start: (config: DlssConfig, exePath?: string) => void
  stop: () => void
}

export function useProcessing(): ProcessingState {
  const [status, setStatus] = useState<ProcessingStatus>('idle')
  const [currentFrame, setCurrentFrame] = useState(0)
  const [totalFrames, setTotalFrames] = useState(0)
  const [errors, setErrors] = useState<string[]>([])

  useEffect(() => {
    setGlobalIsRunning(status === 'running');
  }, [status]);

  useEffect(() => {
    const unlistenProgress = window.dlssApi.onProgress((data) => {
      setCurrentFrame(data.current)
      setTotalFrames(data.total)
    })

    const unlistenError = window.dlssApi.onError((message) => {
      setErrors((prev) => [...prev, message])
      setStatus('error')
    })

    const unlistenComplete = window.dlssApi.onComplete(() => {
      setStatus('done')
    })

    return () => {
      unlistenProgress()
      unlistenError()
      unlistenComplete()
    }
  }, [])

  const progress = totalFrames > 0 ? Math.round((currentFrame / totalFrames) * 100) : 0

  const start = useCallback((config: DlssConfig, exePath?: string) => {
    setStatus('running')
    setCurrentFrame(0)
    setTotalFrames(0)
    setErrors([])
    window.dlssApi.startProcessing(config, exePath)
  }, [])

  const stop = useCallback(() => {
    window.dlssApi.stopProcessing()
    setStatus('idle')
  }, [])

  return { status, progress, currentFrame, totalFrames, errors, start, stop }
}
