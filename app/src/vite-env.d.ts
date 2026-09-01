/// <reference types="vite/client" />

type ProbeResult = {
  imdisk: {ok: boolean; path?: string; message?: string};
  ramdisk: {ok: boolean; letter?: string; root?: string; label?: string; filesystem?: string; message?: string};
  blender: {ok: boolean; path?: string};
  cli: {ok: boolean; path?: string};
};

type SceneCycles = {
  use_adaptive_sampling: boolean;
  adaptive_threshold: number;
  samples: number;
  adaptive_min_samples: number;
  time_limit: number;
  use_denoising: boolean;
  denoiser: string;
  denoiser_label: string;
  preview_denoiser?: string;
  preview_denoiser_label?: string;
  denoising_store_passes?: boolean;
  device?: string;
};

type CyclesCompute = {
  scene_device: string;
  compute_device_type: string;
  devices: Array<{name: string; type: string; use: boolean}>;
};

type SceneState = {
  ok: boolean;
  blend?: string;
  resolution_x: number;
  resolution_y: number;
  resolution_percentage: number;
  frame_start: number;
  frame_end: number;
  fps: number;
  engine: string;
  cycles: SceneCycles | null;
  cycles_compute?: CyclesCompute;
  file_output_count: number;
  file_outputs?: Array<{name: string; label: string; mute?: boolean; directory?: string}>;
  view_transform?: string;
  display_device?: string;
  look?: string;
  view_transforms?: string[];
  display_devices?: string[];
  views_by_display?: Record<string, string[]>;
  error?: string;
};

type SceneOverrides = {
  percentage?: number;
  frameStart?: number;
  frameEnd?: number;
  samples?: number;
  useAdaptiveSampling?: boolean;
  adaptiveThreshold?: number;
  adaptiveMinSamples?: number;
  timeLimit?: number;
  useDenoising?: boolean;
  denoiser?: string;
};

type ExportRequest = {
  blend: string;
  mode: string;
  quality: string;
  frameStart: number;
  frameEnd: number;
  fps?: number;
  outputWidth?: number;
  outputHeight?: number;
  viewTransform?: string;
  displayDevice?: string;
  look?: string;
  outputFormat: string;
  exportDir: string;
  outputTemplate?: string;
  window?: number;
  overrides?: SceneOverrides;
  nativeResBuffers?: boolean;
  nrBuffer?: boolean;
  fg?: number | string;
  effectivePercentage?: number;
  ramWindowFrames?: number;
  /** Input buffer: RAM disk (auto-created in packaged builds) or cache dir. */
  useRamdisk: boolean;
  ramdiskGb?: number;
  cachePath: string;
};

type PipelineEvent = {
  type: string;
  [key: string]: unknown;
};

type LaunchFlags = {
  blend: string | null;
  dcp: string | null;
  home: boolean;
  autoExport: boolean;
  page?: string | null;
  rail?: string | null;
  elevated?: boolean;
  packaged?: boolean;
  elevatePref?: boolean;
  version?: string;
};

type DirEntry = {
  name: string;
  isDir: boolean;
  size: number;
  mtime: number;
};

type NrToolInfo = {
  ok: boolean;
  color_source: string;
  color_kind: 'video' | 'png_sequence';
  buffer_dir: string;
  camera_config: string;
  frames: number;
  width: number;
  height: number;
  buffer_width: number;
  buffer_height: number;
  fps: number;
  error?: string;
};

type NrPreviewResult = {
  ok: boolean;
  frame: number;
  width: number;
  height: number;
  eval_ms: number;
  reset_applied: boolean;
  jpeg_b64?: string;
  interp_jpeg_b64?: string;
  error?: string;
};

interface DlssApi {
  probe: () => Promise<ProbeResult>;
  inspect: (blend: string) => Promise<{ok: boolean; scene?: SceneState; error?: string}>;
  start: (req: ExportRequest) => Promise<{ok: boolean; error?: string}>;
  stop: () => Promise<{ok: boolean; unmounted: false}>;
  nrOpen: (params: {color_source: string; buffer_dir?: string; camera_config?: string}) => Promise<NrToolInfo>;
  nrPreview: (frame: number, settings: Record<string, unknown>) => Promise<NrPreviewResult>;
  nrExport: (params: Record<string, unknown>) => Promise<{ok: boolean; started?: boolean; frames?: number; error?: string}>;
  nrClose: () => Promise<{ok: boolean; error?: string}>;
  nrStop: () => Promise<{ok: boolean; error?: string}>;
  chooseNrVideo: () => Promise<string | null>;
  chooseDir: (title: string) => Promise<string | null>;
  chooseNrVideoSave: (defaultPath?: string) => Promise<string | null>;
  chooseBlend: () => Promise<string | null>;
  chooseExportDir: () => Promise<string | null>;
  chooseDcpOpen: () => Promise<string | null>;
  chooseDcpSave: (defaultPath?: string) => Promise<string | null>;
  loadDcp: (filePath: string) => Promise<{ok: boolean; project?: import('./dcp').DcpProject; error?: string}>;
  saveDcp: (filePath: string, project: import('./dcp').DcpProject) => Promise<{ok: boolean; error?: string}>;
  listRecent: () => Promise<import('./dcp').RecentProject[]>;
  rememberRecent: (filePath: string) => Promise<import('./dcp').RecentProject[]>;
  exists: (filePath: string) => Promise<boolean>;
  openPath: (filePath: string) => Promise<boolean>;
  listDir: (dirPath: string) => Promise<DirEntry[] | null>;
  launchFlags: () => Promise<LaunchFlags>;
  setCliLog: (enabled: boolean) => Promise<void>;
  setElevate: (enabled: boolean) => Promise<void>;
  setTitle: (title: string) => Promise<void>;
  windowMinimize: () => Promise<void>;
  windowMaximize: () => Promise<void>;
  windowClose: () => Promise<void>;
  windowMaximized: () => Promise<boolean>;
  previewDataUrl: (filePath: string) => Promise<string | null>;
  onMaximized: (callback: (maximized: boolean) => void) => () => void;
  onEvent: (callback: (event: PipelineEvent) => void) => () => void;
}

interface Window {
  dlss: DlssApi;
}
