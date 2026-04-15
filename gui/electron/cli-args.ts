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
  }

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

  return args
}
