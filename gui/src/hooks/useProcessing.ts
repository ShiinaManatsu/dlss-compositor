import { useState, useEffect, useCallback } from 'react'
import type { DlssConfig } from '../types/dlss-config'
import { setGlobalIsRunning } from '../state/processing-store'

export type ProcessingStatus = 'idle' | 'running' | 'done' | 'error'

export interface BannerInfo {
  sdk: string
  engine: string
  sr_available: boolean
  fg_available: boolean
  max_multi_frame: number
}

export interface ProcessingState {
  status: ProcessingStatus
  progress: number
  currentFrame: number
  totalFrames: number
  etaSeconds: number | null
  errors: string[]
  latestFramePath: string | null
  start: (config: DlssConfig) => void
  stop: () => void
}

export function useProcessing(): ProcessingState {
  const [status, setStatus] = useState<ProcessingStatus>('idle')
  const [currentFrame, setCurrentFrame] = useState(0)
  const [totalFrames, setTotalFrames] = useState(0)
  const [etaSeconds, setEtaSeconds] = useState<number | null>(null)
  const [startTime, setStartTime] = useState<number | null>(null)
  const [errors, setErrors] = useState<string[]>([])
  const [latestFramePath, setLatestFramePath] = useState<string | null>(null)
  
  useEffect(() => {
    setGlobalIsRunning(status === 'running');
  }, [status]);

  useEffect(() => {
    const unlistenProgress = window.dlssApi.onProgress((data) => {
      setCurrentFrame(data.current)
      setTotalFrames(data.total)
      
      setStartTime(prevStartTime => {
        if (!prevStartTime) return Date.now();
        const elapsedMs = Date.now() - prevStartTime;
        if (data.current >= 3 && elapsedMs > 0) {
          const avgMsPerFrame = elapsedMs / data.current;
          const remainingFrames = data.total - data.current;
          setEtaSeconds(Math.round(avgMsPerFrame * remainingFrames / 1000));
        }
        return prevStartTime;
      });
    })

    const unlistenError = window.dlssApi.onError((message) => {
      setErrors((prev) => [...prev, message])
      setStatus('error')
    })

    const unlistenComplete = window.dlssApi.onComplete(() => {
      setStatus('done')
    })

    const unlistenFrameWritten = window.dlssApi.onFrameWritten((data) => {
      setLatestFramePath(data.path)
    })

    return () => {
      unlistenProgress()
      unlistenError()
      unlistenComplete()
      unlistenFrameWritten()
    }
  }, [])

  const progress = totalFrames > 0 ? Math.round((currentFrame / totalFrames) * 100) : 0

  const start = useCallback((config: DlssConfig) => {
    setStatus('running')
    setCurrentFrame(0)
    setTotalFrames(0)
    setEtaSeconds(null)
    setStartTime(Date.now())
    setErrors([])
    setLatestFramePath(null)
    window.dlssApi.startProcessing(config)
  }, [])

  const stop = useCallback(() => {
    window.dlssApi.stopProcessing()
    setStatus('idle')
  }, [])

  return { status, progress, currentFrame, totalFrames, etaSeconds, errors, latestFramePath, start, stop }
}
