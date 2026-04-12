import { useState, useEffect, useCallback, useRef } from 'react'
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
  const registeredRef = useRef(false)

  useEffect(() => {
    setGlobalIsRunning(status === 'running');
  }, [status]);

  useEffect(() => {
    if (registeredRef.current) return
    registeredRef.current = true

    window.dlssApi.onProgress((data) => {
      setCurrentFrame(data.current)
      setTotalFrames(data.total)
    })

    window.dlssApi.onError((message) => {
      setErrors((prev) => [...prev, message])
      setStatus('error')
    })

    window.dlssApi.onComplete(() => {
      setStatus('done')
    })
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
