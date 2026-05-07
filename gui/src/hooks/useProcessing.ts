import { useState, useEffect, useCallback } from 'react'
import type { DlssConfig } from '../types/dlss-config'
import { setGlobalIsRunning } from '../state/processing-store'
import { useBanner } from '../state/banner-store'

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
  errors: string[]
  latestFramePath: string | null
  start: (config: DlssConfig, exePath?: string) => void
  stop: () => void
}

export function useProcessing(): ProcessingState {
  const [status, setStatus] = useState<ProcessingStatus>('idle')
  const [currentFrame, setCurrentFrame] = useState(0)
  const [totalFrames, setTotalFrames] = useState(0)
  const [errors, setErrors] = useState<string[]>([])
  const [latestFramePath, setLatestFramePath] = useState<string | null>(null)
  
  const { setBanner } = useBanner()

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

    const unlistenFrameWritten = window.dlssApi.onFrameWritten((data) => {
      setLatestFramePath(data.path)
    })

    const unlistenBanner = window.dlssApi.onBanner((data) => {
      setBanner(data)
    })

    return () => {
      unlistenProgress()
      unlistenError()
      unlistenComplete()
      unlistenFrameWritten()
      unlistenBanner()
    }
  }, [])

  const progress = totalFrames > 0 ? Math.round((currentFrame / totalFrames) * 100) : 0

  const start = useCallback((config: DlssConfig, exePath?: string) => {
    setStatus('running')
    setCurrentFrame(0)
    setTotalFrames(0)
    setErrors([])
    setLatestFramePath(null)
    window.dlssApi.startProcessing(config, exePath)
  }, [])

  const stop = useCallback(() => {
    window.dlssApi.stopProcessing()
    setStatus('idle')
  }, [])

  return { status, progress, currentFrame, totalFrames, errors, latestFramePath, start, stop }
}
