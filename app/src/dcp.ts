export const DCP_KIND = 'dlss-compositor-project';
export const DCP_SCHEMA = 1;
export const DEFAULT_FILENAME_TEMPLATE = '{job}_{mode}_v{version}';
export const DEFAULT_SPLITTERS = {queue: 272, inspector: 320} as const;

export const PIN_KEYS = [
  'percentage',
  'frameStart',
  'frameEnd',
  'adaptive',
  'noiseThreshold',
  'samples',
  'minSamples',
  'timeLimit',
  'denoise'
] as const;

export type PinKey = (typeof PIN_KEYS)[number];
export type Pins = Record<PinKey, boolean>;

export const EMPTY_PINS: Pins = {
  percentage: false,
  frameStart: false,
  frameEnd: false,
  adaptive: false,
  noiseThreshold: false,
  samples: false,
  minSamples: false,
  timeLimit: false,
  denoise: false
};

export type Splitters = {
  queue: number;
  inspector: number;
};

export type DcpProject = {
  kind: typeof DCP_KIND;
  schema: number;
  blend: string;
  mode: 'sr' | 'rr';
  pins: Pins;
  percentage: number;
  frameStart: number;
  frameEnd: number;
  fps: number;
  adaptive: boolean;
  noiseThreshold: number;
  samples: number;
  minSamples: number;
  timeLimit: number;
  useDenoising: boolean;
  denoiser: string;
  quality: string;
  outputWidth: number;
  outputHeight: number;
  outputFormat: string;
  exportDir: string;
  filenameTemplate: string;
  outputVersion: number;
  windowSize: number;
  nativeResBuffers: boolean;
  /** 0 = off; 2..6 matches CLI `--fg` / RPC `fg` / `fg_multiplier`. */
  fg: number;
  splitters: Splitters;
  preview?: string | null;
};

export type RecentProject = {
  path: string;
  name: string;
  mode: string;
  mtime: number;
  preview: string | null;
};

export type ParseDcpResult = {ok: true; project: DcpProject} | {ok: false; error: string};

const QUEUE_MIN = 180;
const QUEUE_MAX = 520;
const INSPECTOR_MIN = 240;
const INSPECTOR_MAX = 520;

export function basename(filePath: string) {
  return filePath.replace(/\\/g, '/').split('/').filter(Boolean).pop() || filePath;
}

export function projectTitle(dcpPath: string | null | undefined) {
  if (!dcpPath) return 'DLSS Compositor';
  return basename(dcpPath).replace(/\.dcp$/i, '') || 'DLSS Compositor';
}

export function rrAllowed(engine: string | undefined | null) {
  return String(engine || '').toUpperCase() === 'CYCLES';
}

/** Native-res dual pass is Cycles + SR only. RR is already one output-res pass. */
export function nativeResAllowed(engine: string | undefined | null, mode: string) {
  return rrAllowed(engine) && String(mode || '').toLowerCase() === 'sr';
}

const COMPUTE_LABELS: Record<string, string> = {
  OPTIX: 'OptiX',
  CUDA: 'CUDA',
  HIP: 'HIP',
  METAL: 'Metal',
  ONEAPI: 'oneAPI'
};

export function computeDeviceLabel(code: string | undefined | null) {
  const raw = String(code || '').trim();
  if (!raw) return '';
  return COMPUTE_LABELS[raw.toUpperCase()] || raw;
}

export function cyclesDeviceMeta(
  device: string | undefined | null,
  computeType: string | undefined | null
) {
  const dev = String(device || '').trim();
  const compute = computeDeviceLabel(computeType);
  if (!dev && !compute) return '';
  if (dev && compute) return `设备 ${dev}/${compute}`;
  if (dev) return `设备 ${dev}`;
  return `设备 ${compute}`;
}

export function cyclesDenoiseMeta(cycles: {
  denoiser?: string;
  denoiser_label?: string;
  preview_denoiser?: string;
  preview_denoiser_label?: string;
} | null | undefined) {
  if (!cycles) return '';
  const render = String(cycles.denoiser_label || cycles.denoiser || '').trim();
  const preview = String(cycles.preview_denoiser_label || cycles.preview_denoiser || '').trim();
  if (!render) return '';
  if (preview && preview !== render) return `降噪 ${render} · 视口 ${preview}`;
  return `降噪 ${render}`;
}

/** Frame Generation is available for SR and RR (EEVEE stays on the SR path). */
export function fgAllowed(mode: string | undefined | null) {
  const value = String(mode || '').toLowerCase();
  return value === 'sr' || value === 'rr';
}

/** Empty / omitted / 关 → 0. Accepts 2, '2', '2x'..'6x'. Invalid values stay off. */
export function parseFg(value: unknown): number {
  if (value == null || value === '') return 0;
  if (typeof value === 'boolean') return 0;
  let raw = 0;
  if (typeof value === 'number' && Number.isFinite(value)) {
    raw = Math.trunc(value);
  } else if (typeof value === 'string') {
    let text = value.trim().toLowerCase();
    if (text === '0' || text === 'off' || text === 'none' || text === '关') return 0;
    if (text.endsWith('x')) text = text.slice(0, -1);
    if (!text) return 0;
    const parsed = Number.parseInt(text, 10);
    if (!Number.isFinite(parsed)) return 0;
    raw = parsed;
  } else {
    return 0;
  }
  if (raw === 0) return 0;
  if (raw < 2 || raw > 6) return 0;
  return raw;
}

/** CLI/RPC payload. Off → omit (caller should not send fg=0 unless they want it explicit). */
export function fgRpcValue(fg: number): string | undefined {
  const multiplier = parseFg(fg);
  return multiplier >= 2 ? `${multiplier}x` : undefined;
}

/** Output frames = N when off; N * fg - (fg - 1) when 2x..6x. */
export function outputFrameCount(inputFrames: number, fg: number) {
  const n = Math.max(1, Math.trunc(inputFrames));
  const multiplier = parseFg(fg);
  if (multiplier < 2) return n;
  return multiplier * n - (multiplier - 1);
}

export function clampSplitters(input: Partial<Splitters> | null | undefined): Splitters {
  const queue = clampNumber(input?.queue, DEFAULT_SPLITTERS.queue, QUEUE_MIN, QUEUE_MAX);
  const inspector = clampNumber(input?.inspector, DEFAULT_SPLITTERS.inspector, INSPECTOR_MIN, INSPECTOR_MAX);
  return {queue, inspector};
}

export function nextOutputVersion(current: number) {
  const n = Number.isFinite(current) ? Math.trunc(current) : 1;
  return Math.max(1, n) + 1;
}

const KNOWN_EXTS = new Set(['.png', '.exr', '.mov', '.mp4']);
const FRAME_TOKEN = /\{frame(?::0?\d+d)?\}/;
const PLACEHOLDER = /\{([^{}]+)\}/g;

export type FilenameContext = {
  job: string;
  mode: string;
  version: number;
  format: string;
};

export type ResolveTemplateResult = {ok: true; template: string} | {ok: false; error: string};

export function isSequenceFormat(format: string) {
  const value = format.toLowerCase();
  return value === 'png' || value === 'png_16' || value === 'exr' || value === 'exr_linear';
}

export function extensionForFormat(format: string) {
  const value = format.toLowerCase();
  if (value === 'png' || value === 'png_16') return '.png';
  if (value === 'exr' || value === 'exr_linear') return '.exr';
  if (value === 'h265' || value === 'hevc' || value === 'h265_hevc') return '.mp4';
  return '.mov';
}

export function jobTokenFromBlend(blend: string) {
  return sanitizeFilenameToken(basename(blend).replace(/\.blend$/i, '')) || 'job';
}

export function resolveFilenameTemplate(template: string, ctx: FilenameContext): ResolveTemplateResult {
  const raw = template.trim();
  if (!raw) {
    return {ok: false, error: '文件名模板为空'};
  }
  const job = sanitizeFilenameToken(ctx.job) || 'job';
  const mode = sanitizeFilenameToken(ctx.mode.toLowerCase()) || 'sr';
  const version = Math.max(1, Math.trunc(Number.isFinite(ctx.version) ? ctx.version : 1));
  let out = raw
    .replaceAll('{job}', job)
    .replaceAll('{mode}', mode)
    .replaceAll('{version}', String(version));
  out = out.replaceAll('{frame}', '{frame:04d}');

  const leftover: string[] = [];
  out.replace(PLACEHOLDER, (token, name: string) => {
    if (!/^frame(?::0?\d+d)?$/.test(name)) leftover.push(token);
    return token;
  });
  if (leftover.length) {
    return {ok: false, error: `文件名模板无法解析: ${leftover.join(' ')}`};
  }

  const sequence = isSequenceFormat(ctx.format);
  if (!sequence) {
    out = out.replace(/#+/g, '').replace(new RegExp(FRAME_TOKEN.source, 'g'), '');
    out = collapseUnderscores(out);
  } else if (!FRAME_TOKEN.test(out) && !out.includes('#')) {
    out = appendBeforeExtension(out, '_{frame:04d}');
  }

  out = collapseUnderscores(out);
  if (/[{}]/.test(out.replace(new RegExp(FRAME_TOKEN.source, 'g'), ''))) {
    return {ok: false, error: '文件名模板无法解析'};
  }
  const stem = stripKnownExtension(out);
  if (!stem) {
    return {ok: false, error: '文件名模板无法解析'};
  }
  return {ok: true, template: applyFormatExtension(out, extensionForFormat(ctx.format))};
}

function sanitizeFilenameToken(value: string) {
  return value.replace(/[<>:"/\\|?*\u0000-\u001f]/g, '_').replace(/[. ]+$/g, '').trim();
}

function collapseUnderscores(value: string) {
  return value.replace(/_+/g, '_').replace(/^_+/, '').replace(/_+$/, '').replace(/_+(?=\.[^.]+$)/, '');
}

function stripKnownExtension(value: string) {
  const dot = value.lastIndexOf('.');
  if (dot >= 0 && KNOWN_EXTS.has(value.slice(dot).toLowerCase())) {
    return value.slice(0, dot);
  }
  return value;
}

function appendBeforeExtension(value: string, extra: string) {
  const dot = value.lastIndexOf('.');
  if (dot >= 0 && KNOWN_EXTS.has(value.slice(dot).toLowerCase())) {
    return `${value.slice(0, dot)}${extra}${value.slice(dot)}`;
  }
  return `${value}${extra}`;
}

function applyFormatExtension(name: string, ext: string) {
  const dot = name.lastIndexOf('.');
  if (dot >= 0 && KNOWN_EXTS.has(name.slice(dot).toLowerCase())) {
    return name.slice(0, dot) + ext;
  }
  return name + ext;
}

export function parseDcp(raw: string): ParseDcpResult {
  let data: unknown;
  try {
    data = JSON.parse(raw);
  } catch {
    return {ok: false, error: '不是有效 JSON'};
  }
  if (!data || typeof data !== 'object' || Array.isArray(data)) {
    return {ok: false, error: '工程文件格式不对'};
  }
  const obj = data as Record<string, unknown>;
  if (obj.kind != null && obj.kind !== DCP_KIND) {
    return {ok: false, error: '不是 DLSS Compositor 工程'};
  }
  if (typeof obj.blend !== 'string' || !obj.blend.trim()) {
    return {ok: false, error: '缺少 blend 路径'};
  }
  if (obj.mode !== 'sr' && obj.mode !== 'rr') {
    return {ok: false, error: '缺少模式 SR/RR'};
  }
  return {
    ok: true,
    project: {
      kind: DCP_KIND,
      schema: DCP_SCHEMA,
      blend: obj.blend,
      mode: obj.mode,
      pins: parsePins(obj.pins),
      percentage: num(obj.percentage, 100),
      frameStart: num(obj.frameStart, 1),
      frameEnd: num(obj.frameEnd, 1),
      fps: num(obj.fps, 24),
      adaptive: bool(obj.adaptive, true),
      noiseThreshold: num(obj.noiseThreshold, 0.01),
      samples: num(obj.samples, 0),
      minSamples: num(obj.minSamples, 0),
      timeLimit: num(obj.timeLimit, 0),
      useDenoising: bool(obj.useDenoising, false),
      denoiser: str(obj.denoiser, 'OPENIMAGEDENOISE'),
      quality: str(obj.quality, 'BALANCED'),
      outputWidth: num(obj.outputWidth, 1920),
      outputHeight: num(obj.outputHeight, 1080),
      outputFormat: str(obj.outputFormat, 'prores_422hq'),
      exportDir: str(obj.exportDir, 'build/stage4-out'),
      filenameTemplate: str(obj.filenameTemplate, DEFAULT_FILENAME_TEMPLATE),
      outputVersion: Math.max(1, Math.trunc(num(obj.outputVersion, 1))),
      windowSize: num(obj.windowSize, 6),
      nativeResBuffers: bool(obj.nativeResBuffers, false),
      fg: parseFg(obj.fg ?? obj.fg_multiplier),
      splitters: clampSplitters(isRecord(obj.splitters) ? (obj.splitters as Partial<Splitters>) : undefined),
      preview: typeof obj.preview === 'string' && obj.preview ? obj.preview : null
    }
  };
}

export function serializeDcp(project: DcpProject): string {
  const body: DcpProject = {
    ...project,
    kind: DCP_KIND,
    schema: DCP_SCHEMA,
    mode: project.mode === 'rr' ? 'rr' : 'sr',
    pins: {...EMPTY_PINS, ...project.pins},
    splitters: clampSplitters(project.splitters),
    filenameTemplate: project.filenameTemplate || DEFAULT_FILENAME_TEMPLATE,
    outputVersion: Math.max(1, Math.trunc(project.outputVersion || 1)),
    fg: parseFg(project.fg),
    preview: project.preview || null
  };
  return `${JSON.stringify(body, null, 2)}\n`;
}

export function defaultDcpPath(blend: string) {
  const i = Math.max(blend.lastIndexOf('\\'), blend.lastIndexOf('/'));
  const dir = i >= 0 ? blend.slice(0, i + 1) : '';
  const file = (i >= 0 ? blend.slice(i + 1) : blend).replace(/\.blend$/i, '');
  return `${dir}${file}.dcp`;
}

export function emptyProject(blend: string): DcpProject {
  return {
    kind: DCP_KIND,
    schema: DCP_SCHEMA,
    blend,
    mode: 'sr',
    pins: {...EMPTY_PINS},
    percentage: 100,
    frameStart: 1,
    frameEnd: 1,
    fps: 24,
    adaptive: true,
    noiseThreshold: 0.01,
    samples: 0,
    minSamples: 0,
    timeLimit: 0,
    useDenoising: false,
    denoiser: 'OPENIMAGEDENOISE',
    quality: 'BALANCED',
    outputWidth: 1920,
    outputHeight: 1080,
    outputFormat: 'prores_422hq',
    exportDir: 'build/stage4-out',
    filenameTemplate: DEFAULT_FILENAME_TEMPLATE,
    outputVersion: 1,
    windowSize: 6,
    nativeResBuffers: false,
    fg: 0,
    splitters: {...DEFAULT_SPLITTERS},
    preview: null
  };
}

function parsePins(raw: unknown): Pins {
  const pins = {...EMPTY_PINS};
  if (!isRecord(raw)) return pins;
  for (const key of PIN_KEYS) {
    if (typeof raw[key] === 'boolean') pins[key] = raw[key];
  }
  return pins;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return Boolean(value) && typeof value === 'object' && !Array.isArray(value);
}

function num(value: unknown, fallback: number) {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback;
}

function bool(value: unknown, fallback: boolean) {
  return typeof value === 'boolean' ? value : fallback;
}

function str(value: unknown, fallback: string) {
  return typeof value === 'string' && value ? value : fallback;
}

function clampNumber(value: unknown, fallback: number, min: number, max: number) {
  const n = typeof value === 'number' && Number.isFinite(value) ? value : fallback;
  return Math.min(max, Math.max(min, Math.round(n)));
}
