export interface DlssConfig {
  inputDir: string;
  outputDir: string;
  scaleEnabled: boolean;
  scaleFactor: number;
  quality: 'DLAA' | 'MaxQuality' | 'Balanced' | 'Performance' | 'UltraPerformance';
  preset: 'J' | 'K' | 'L' | 'M';
  interpolateFactor: 0 | 2 | 4;
  cameraDataFile: string;
  memoryBudgetGB: number;
  encodeVideo: boolean;
  videoOutputFile: string;
  fps: number;
  exrCompression: 'none' | 'zip' | 'zips' | 'piz' | 'dwaa' | 'dwab';
  exrDwaQuality: number;
  outputPasses: ('beauty' | 'depth' | 'normals')[];
  tonemapMode: 'none' | 'pq' | 'custom';
  inverseTonemapEnabled: boolean;
  forwardLutFile: string;
  inverseLutFile: string;
  channelMapFile: string;
  sourceFps: number;
  sharpness: number;
  // Per-quality presets (Plan 1 T1) — 'auto' uses NVIDIA recommended; others override
  presetDLAA: 'auto' | 'J' | 'K' | 'L' | 'M';
  presetQuality: 'auto' | 'J' | 'K' | 'L' | 'M';
  presetBalanced: 'auto' | 'J' | 'K' | 'L' | 'M';
  presetPerformance: 'auto' | 'J' | 'K' | 'L' | 'M';
  presetUltraPerformance: 'auto' | 'J' | 'K' | 'L' | 'M';
  presetUltraQuality: 'auto' | 'J' | 'K' | 'L' | 'M';
  // NGX advanced flags (Plan 1 T2)
  featureFlagHDR: boolean;        // default true — NVSDK_NGX_DLSS_Feature_Flags_IsHDR
  featureFlagMVLowRes: boolean;   // default true — MVLowRes flag
  featureFlagAutoExposure: boolean; // default true — AutoExposure flag
  enableOutputSubrects: boolean;  // default false
  exposureScale: number;          // default 1.0 — InExposureScale
  frameTimeDeltaMsOverride: number; // default -1.0 (-1 = auto from --source-fps)
  dlssSharpness: number;          // default 0.0 — NGX InSharpness (distinct from post-CAS sharpness)
  // Camera cut detection (Plan 1 T3)
  cameraCutDetectionEnabled: boolean;     // default true
  cameraCutTranslationThreshold: number;  // default 5.0 world units
  cameraCutRotationThreshold: number;     // default 30.0 degrees
  // FG-related knob
  enableDynamicResolution: boolean;       // default false
  ngxLogLevel: 'off' | 'error' | 'warn' | 'info' | 'verbose';
  sourceWidth: number | null;
  sourceHeight: number | null;
  targetPreset: '4K' | '1440p' | '1080p' | 'Custom' | 'DLAA';
  customTargetWidth: number | null;
  customTargetHeight: number | null;
}

export const DEFAULT_CONFIG: DlssConfig = {
  inputDir: '',
  outputDir: '',
  scaleEnabled: true,
  scaleFactor: 2.0,
  quality: 'MaxQuality',
  preset: 'L',
  interpolateFactor: 0,
  cameraDataFile: '',
  memoryBudgetGB: 8,
  encodeVideo: false,
  videoOutputFile: 'output.mp4',
  fps: 24,
  exrCompression: 'dwaa',
  exrDwaQuality: 95.0,
  outputPasses: ['beauty'],
  tonemapMode: 'pq',
  inverseTonemapEnabled: true,
  forwardLutFile: '',
  inverseLutFile: '',
  channelMapFile: '',
  sourceFps: 30,
  sharpness: 0.0,
  presetDLAA: 'auto',
  presetQuality: 'auto',
  presetBalanced: 'auto',
  presetPerformance: 'auto',
  presetUltraPerformance: 'auto',
  presetUltraQuality: 'auto',
  featureFlagHDR: true,
  featureFlagMVLowRes: true,
  featureFlagAutoExposure: true,
  enableOutputSubrects: false,
  exposureScale: 1.0,
  frameTimeDeltaMsOverride: -1.0,
  dlssSharpness: 0.0,
  cameraCutDetectionEnabled: true,
  cameraCutTranslationThreshold: 5.0,
  cameraCutRotationThreshold: 30.0,
  enableDynamicResolution: false,
  ngxLogLevel: 'warn',
  sourceWidth: null,
  sourceHeight: null,
  targetPreset: '4K',
  customTargetWidth: null,
  customTargetHeight: null,
}

export interface Settings {
  lastInputDir: string;
  lastOutputDir: string;
  config: Partial<DlssConfig>;
  windowBounds?: { x: number; y: number; width: number; height: number };
}
