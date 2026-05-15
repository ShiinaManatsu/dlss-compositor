import type { DlssConfig } from '../src/types/dlss-config'

export function buildCliArgs(config: DlssConfig, _exePath: string): string[] {
  const args = [
    '--input-dir',
    config.inputDir,
    '--output-dir',
    config.outputDir,
  ]

  if (config.scaleEnabled) {
    args.push('--scale', String(config.scaleFactor), '--quality', config.quality, '--preset', config.preset)
  }

  if (config.interpolateFactor > 0) {
    args.push('--interpolate', `${config.interpolateFactor}x`)

    if (config.cameraDataFile) {
      args.push('--camera-data', config.cameraDataFile)
    }
  }

  if (config.memoryBudgetGB !== 8) {
    args.push('--memory-budget', String(config.memoryBudgetGB))
  }

  if (config.encodeVideo && config.videoOutputFile) {
    args.push('--encode-video', config.videoOutputFile, '--fps', String(config.fps))
  }

  if (config.exrCompression !== 'dwaa') {
    args.push('--exr-compression', config.exrCompression)
  }

  if (
    (config.exrCompression === 'dwaa' || config.exrCompression === 'dwab')
    && config.exrDwaQuality !== 95.0
  ) {
    args.push('--exr-dwa-quality', String(config.exrDwaQuality))
  }

  const extraPasses = config.outputPasses?.filter((pass) => pass !== 'beauty') ?? []
  if (extraPasses.length > 0) {
    args.push('--output-passes', extraPasses.join(','))
  }

  if (config.tonemapMode === 'none') {
    args.push('--tonemap', 'none')
  } else if (config.tonemapMode === 'custom') {
    args.push('--tonemap', 'custom')
  }
  // 'pq' is CLI default — no need to pass explicitly

  if (!config.inverseTonemapEnabled) {
    args.push('--no-inverse-tonemap')
  }

  if (config.forwardLutFile) {
    args.push('--tonemap-lut', config.forwardLutFile)
  }

  if (config.inverseLutFile) {
    args.push('--inverse-tonemap-lut', config.inverseLutFile)
  }

  if (config.channelMapFile) {
    args.push('--channel-map', config.channelMapFile)
  }

  // Source FPS — always pass (used by DLSS frame timing)
  args.push('--source-fps', String(config.sourceFps))

  // Sharpness — only if > 0 (0 = off)
  if (config.sharpness > 0) {
    args.push('--sharpness', config.sharpness.toFixed(2))
  }

  // NGX log level — only if not default 'warn'
  if (config.ngxLogLevel !== 'warn') {
    args.push('--ngx-log-level', config.ngxLogLevel)
  }

  // Per-quality presets — only emit non-auto values
  if (config.presetDLAA !== 'auto') args.push('--preset-dlaa', config.presetDLAA)
  if (config.presetQuality !== 'auto') args.push('--preset-quality', config.presetQuality)
  if (config.presetBalanced !== 'auto') args.push('--preset-balanced', config.presetBalanced)
  if (config.presetPerformance !== 'auto') args.push('--preset-performance', config.presetPerformance)
  if (config.presetUltraPerformance !== 'auto') args.push('--preset-ultra-performance', config.presetUltraPerformance)
  if (config.presetUltraQuality !== 'auto') args.push('--preset-ultra-quality', config.presetUltraQuality)

  // Feature flags — emit --no-* when disabled (defaults are all true)
  if (!config.featureFlagHDR) args.push('--no-feature-flag-hdr')
  if (!config.featureFlagMVLowRes) args.push('--no-feature-flag-mv-low-res')
  if (!config.featureFlagAutoExposure) args.push('--no-feature-flag-auto-exposure')
  if (config.enableOutputSubrects) args.push('--enable-output-subrects')

  // Numeric overrides — emit when differ from default
  if (config.exposureScale !== 1.0) args.push('--exposure-scale', String(config.exposureScale))
  if (config.frameTimeDeltaMsOverride > 0) args.push('--frame-time-delta-ms', String(config.frameTimeDeltaMsOverride))
  if (config.dlssSharpness > 0) args.push('--dlss-sharpness', config.dlssSharpness.toFixed(2))
  if (config.enableDynamicResolution) args.push('--enable-dynamic-resolution')

  // Camera cut detection
  if (!config.cameraCutDetectionEnabled) args.push('--no-camera-cut-detection')
  if (config.cameraCutTranslationThreshold !== 5.0) args.push('--camera-cut-translation-threshold', String(config.cameraCutTranslationThreshold))
  if (config.cameraCutRotationThreshold !== 30.0) args.push('--camera-cut-rotation-threshold', String(config.cameraCutRotationThreshold))

  // GUI always uses NDJSON for reliable event parsing
  args.push('--progress-format', 'json')

  return args
}
