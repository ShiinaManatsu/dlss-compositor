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
  ngxLogLevel: 'warn',
  sourceWidth: null,
  sourceHeight: null,
  targetPreset: '4K',
  customTargetWidth: null,
  customTargetHeight: null,
}

export interface Settings {
  exePath: string;
  lastInputDir: string;
  lastOutputDir: string;
  config: Partial<DlssConfig>;
  windowBounds?: { x: number; y: number; width: number; height: number };
}
