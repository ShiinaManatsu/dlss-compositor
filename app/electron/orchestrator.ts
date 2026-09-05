import {spawn, type ChildProcess} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {app} from 'electron';
import {parseFg} from '../src/dcp';
import {userErrorMessage} from '../src/errors';
import {estimateWindowFrameBytes, phaseLabel, planRamWindows, ramWindowBudget} from '../src/ramWindows';
import {StdioRpcClient} from './rpc';
import {createRamdisk, discoverRamdisk, findImdisk, IMDISK_MISSING, unmountRamdisk, VOLUME_LABEL} from './ramdisk';

/**
 * Packaged apps resolve the bundled backend (cli + pipeline scripts) under
 * resources/ and write logs into userData; dev builds default to the repo.
 */
export type OrchestratorPaths = {
  backend?: string;
  logs?: string;
};

let backendDir: string | null = null;
let logDir: string | null = null;

export function configureOrchestratorPaths(paths: OrchestratorPaths): void {
  backendDir = paths.backend ?? null;
  logDir = paths.logs ?? null;
}

function backendRoot(): string {
  return backendDir ?? repoRoot();
}

function logRoot(): string {
  return logDir ?? path.join(repoRoot(), 'build', 'stage4-out');
}

function debug(message: string): void {
  try {
    const log = path.join(logRoot(), 'app-debug.log');
    fs.mkdirSync(path.dirname(log), {recursive: true});
    fs.appendFileSync(log, `${new Date().toISOString()} ${message}\n`);
  } catch {
    /* ignore */
  }
}

export type EngineMode = 'sr' | 'rr';

export type SceneOverrides = {
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

export type SceneCycles = {
  use_adaptive_sampling: boolean;
  adaptive_threshold: number;
  samples: number;
  adaptive_min_samples: number;
  time_limit: number;
  use_denoising: boolean;
  denoiser: string;
  denoiser_label: string;
  denoising_store_passes?: boolean;
  device?: string;
};

export type SceneState = {
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

export type ExportRequest = {
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
  blender?: string;
  cli?: string;
  overrides?: SceneOverrides;
  percentage?: number;
  samples?: number;
  nativeResBuffers?: boolean;
  /** Export dlssnrbuffer_####.exr (MV+depth guidance) for the NR tool. */
  nrBuffer?: boolean;
  /** Session perf fallbacks for inject.py (defaults match its fallback set). */
  perfPersistent?: boolean;
  perfCameraCull?: boolean;
  cameraCullMargin?: number;
  perfDistanceCull?: boolean;
  distanceCullMargin?: number;
  perfTileSize?: number;
  /** 分段并行渲染：段数（2..6）；缺省/1 = 关（单实例连续渲染）。 */
  parallelSegments?: number;
  /** 分段预热帧：段首往前多渲染的历史积累帧（默认 16），输出时丢弃。 */
  parallelWarmup?: number;
  fg?: number | string;
  effectivePercentage?: number;
  ramWindowFrames?: number;
  /** Input buffer: RAM disk (auto-created when packaged) or a plain cache dir. */
  useRamdisk: boolean;
  /** RAM-disk size in GB when the app auto-creates one (default 2, clamped 1..64). */
  ramdiskGb?: number;
  cachePath: string;
};

export type ProbeResult = {
  imdisk: {ok: boolean; path?: string; message?: string};
  ramdisk: {
    ok: boolean;
    letter?: string;
    root?: string;
    label?: string;
    filesystem?: string;
    message?: string;
    totalBytes?: number;
    freeBytes?: number;
  };
  blender: {ok: boolean; path?: string};
  cli: {ok: boolean; path?: string};
};

export type PipelineEvent = {
  type: string;
  [key: string]: unknown;
};

/** One parallel segment: its own Blender (EXR producer) + CLI (NGX consumer). */
type SegRuntime = {
  index: number;
  cli: ChildProcess | null;
  rpc: StdioRpcClient | null;
  blender: ChildProcess | null;
  stderrFd: number | null;
  bufferDir: string;
  segVideoPath: string | null;   // ProRes/H265 segments; null for sequence output
  segStart: number;
  segEnd: number;
  renderStart: number;
  /** Outputs written by earlier segments — CLI progress is absolute-indexed. */
  outputOffset: number;
  expectedOutputs: number;
  outputsDone: number;
  done: boolean;
  failed: boolean;
  cancelled: boolean;
  errorDetail: string;
};

const NR_RESERVED = 'DLSS5-NR 模型未到，接口已预留';

/** RR 渲染比例（dcp.ts RR_RECOMMENDED_PERCENT 同步）：性能档 51% 避开
 *  Blender floor(50%) 比 NGX ceil 下限低 1px；超高质量/超高性能不为 RR 提供百分
 *  比 —— start() 直接拒绝。 */
const RR_QUALITY_PERCENT: Record<string, number> = {
  DLAA: 100,
  QUALITY: 67,
  BALANCED: 58,
  PERFORMANCE: 51,
};

const RR_QUALITY_IDS = new Set(Object.keys(RR_QUALITY_PERCENT));

const BLENDER_CANDIDATES = [
  path.join('D:', 'Games', 'SteamLibrary', 'steamapps', 'common', 'Blender', 'blender.exe'),
  'C:\\Program Files\\Blender Foundation\\Blender 5.2\\blender.exe',
  'C:\\Program Files\\Blender Foundation\\Blender 5.1\\blender.exe',
];

const CLI_CANDIDATES = [
  'build/x64-windows-msvc-release/Release/dlss-compositor-cli.exe',
  'build/x64-windows-msvc-debug/Debug/dlss-compositor-cli.exe',
  'build/review/Release/dlss-compositor-cli.exe',
  'build/Release/dlss-compositor-cli.exe',
];

/** Drive roots like `S:\` already exist; mkdirSync throws EPERM on them. */
function ensureDir(dir: string): void {
  const resolved = path.resolve(dir);
  const root = path.parse(resolved).root;
  if (!resolved || resolved === root) return;
  fs.mkdirSync(resolved, {recursive: true});
}

function repoRoot(): string {
  const here = path.dirname(fileURLToPath(import.meta.url));
  return path.resolve(here, '..', '..');
}

function existsFile(file: string): boolean {
  try {
    return fs.statSync(file).isFile();
  } catch {
    return false;
  }
}

function killTree(proc: ChildProcess | null): void {
  if (!proc || proc.exitCode !== null || proc.pid == null) return;
  if (process.platform === 'win32') {
    spawn('taskkill', ['/PID', String(proc.pid), '/T', '/F'], {stdio: 'ignore'});
  } else {
    proc.kill('SIGTERM');
  }
}

function findBlender(explicit?: string): string | null {
  if (explicit && existsFile(explicit)) return explicit;
  const env = process.env.BLENDER_EXE;
  if (env && existsFile(env)) return env;
  for (const candidate of BLENDER_CANDIDATES) {
    if (existsFile(candidate)) return candidate;
  }
  return null;
}

function findCli(explicit?: string, backend?: string): string | null {
  if (explicit && existsFile(explicit)) return explicit;
  const env = process.env.DLSS_COMPOSITOR_CLI;
  if (env && existsFile(env)) return env;
  if (backend) {
    // Packaged: exactly the bundled CLI, nothing else.
    const bundled = path.join(backend, 'cli', 'dlss-compositor-cli.exe');
    return existsFile(bundled) ? bundled : null;
  }
  const root = repoRoot();
  for (const candidate of CLI_CANDIDATES) {
    const full = path.isAbsolute(candidate) ? candidate : path.join(root, candidate);
    if (existsFile(full)) return full;
  }
  return null;
}

function pushOverrideArgs(args: string[], ov: SceneOverrides): void {
  if (ov.percentage != null) args.push('--percentage', String(ov.percentage));
  if (ov.frameStart != null) args.push('--frame-start', String(ov.frameStart));
  if (ov.frameEnd != null) args.push('--frame-end', String(ov.frameEnd));
  if (ov.samples != null) args.push('--samples', String(ov.samples));
  if (ov.useAdaptiveSampling != null) {
    args.push('--use-adaptive-sampling', ov.useAdaptiveSampling ? 'true' : 'false');
  }
  if (ov.adaptiveThreshold != null) args.push('--adaptive-threshold', String(ov.adaptiveThreshold));
  if (ov.adaptiveMinSamples != null) args.push('--adaptive-min-samples', String(ov.adaptiveMinSamples));
  if (ov.timeLimit != null) args.push('--time-limit', String(ov.timeLimit));
  if (ov.useDenoising != null) args.push('--use-denoising', ov.useDenoising ? 'true' : 'false');
  if (ov.denoiser) args.push('--denoiser', ov.denoiser);
}

function mergeOverrides(req: ExportRequest): SceneOverrides {
  return {
    percentage: req.overrides?.percentage ?? req.percentage,
    frameStart: req.overrides?.frameStart,
    frameEnd: req.overrides?.frameEnd,
    samples: req.overrides?.samples ?? req.samples,
    useAdaptiveSampling: req.overrides?.useAdaptiveSampling,
    adaptiveThreshold: req.overrides?.adaptiveThreshold,
    adaptiveMinSamples: req.overrides?.adaptiveMinSamples,
    timeLimit: req.overrides?.timeLimit,
    useDenoising: req.mode === 'rr' ? undefined : req.overrides?.useDenoising,
    denoiser: req.mode === 'rr' ? undefined : req.overrides?.denoiser,
  };
}

async function runCommand(
  exe: string,
  args: string[],
  timeoutMs: number,
  cwd: string = backendRoot()
): Promise<{code: number; stdout: string; stderr: string}> {
  return new Promise((resolve, reject) => {
    const proc = spawn(exe, args, {
      cwd,
      windowsHide: true,
    });
    let stdout = '';
    let stderr = '';
    proc.stdout?.on('data', (chunk) => {
      stdout += chunk.toString('utf8');
    });
    proc.stderr?.on('data', (chunk) => {
      stderr += chunk.toString('utf8');
    });
    const timer = setTimeout(() => {
      killTree(proc);
      reject(new Error(`${path.basename(exe)} timeout`));
    }, timeoutMs);
    proc.on('error', (err) => {
      clearTimeout(timer);
      reject(err);
    });
    proc.on('close', (code) => {
      clearTimeout(timer);
      resolve({code: code ?? 1, stdout, stderr});
    });
  });
}

/** Per-segment video filename: `<stem>__seg<i><ext>` next to the final output. */
function segVideoName(exportDir: string, outputTemplate: string, index: number): string {
  const dot = outputTemplate.lastIndexOf('.');
  const stem = dot > 0 ? outputTemplate.slice(0, dot) : outputTemplate;
  const ext = dot > 0 ? outputTemplate.slice(dot) : '';
  return path.join(exportDir, `${stem}__seg${index}${ext}`);
}

export class PipelineOrchestrator {
  /**
   * @param autoRamdisk packaged builds auto-create/unmount the DLSSRAM disk
   * around exports; dev builds expect a manually mounted disk (no admin needed).
   * Override with DC_RAMDISK_AUTO=1 / =0.
   */
  constructor(private autoRamdisk = false) {}

  private listeners = new Set<(event: PipelineEvent) => void>();
  private blender: ChildProcess | null = null;
  private cli: ChildProcess | null = null;
  private rpc: StdioRpcClient | null = null;
  /** Dedicated CLI process for the NR/FG tool. Process isolation keeps the
   *  tool independent of the job lifecycle (its NGX/Vulkan state and running
   *  guard are per-process): the tool opens while a job runs — or winds down
   *  after a stop — and the job's stop can never hit a tool export. */
  private nrCli: ChildProcess | null = null;
  private nrRpc: StdioRpcClient | null = null;
  private nrCliPath: string | null = null;
  private nrStderrFd: number | null = null;
  private pollTimer: NodeJS.Timeout | null = null;
  private nrPollTimer: NodeJS.Timeout | null = null;
  private nrPollInFlight = false;
  private nrStopRequested = false;
  private running = false;
  private ramLetter: string | null = null;
  private cliPath: string | null = null;
  private stderrFd: number | null = null;
  /** CLI stderr file (cli.stderr.log): dev default on, release default off
   *  (settings toggle). When off, stderr goes to the void. */
  private cliLog = !app.isPackaged;

  /** Settings toggle for the CLI debug log; takes effect on the next spawn. */
  setCliLog(enabled: boolean): void {
    this.cliLog = enabled;
  }
  private session = 0;
  private pollInFlight = false;
  private exportSettled = false;
  /** Set while a user stop is draining (writer close + mov trailer); pollOnce
   *  keeps polling and emits the single 'stopped' when the CLI settles. */
  private stopping = false;
  private stopDeadline = 0;
  private ramDir: string | null = null;
  private nrBufferDir: string | null = null;
  private ramdiskManaged = false;
  private lastRamdiskBytes = 0;
  // ── Segmented parallel rendering state ─────────────────────────────────────
  private segs: SegRuntime[] = [];
  private segTimer: NodeJS.Timeout | null = null;
  private segPollInFlight = false;
  private segOutputsTotal = 0;
  private segLastOutputs = -1;
  private segLastProgressAt = 0;
  private segLastSyntheticFrame = 0;
  private segJobRange = {start: 0, end: 0, totalInputs: 0};
  private segFinalVideoPath: string | null = null;
  private segLineBufs = new Map<number, string>();
  private segStopDeadline = 0;

  onEvent(listener: (event: PipelineEvent) => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private emit(event: PipelineEvent): void {
    for (const listener of this.listeners) listener(event);
  }

  async probe(): Promise<ProbeResult> {
    const blender = findBlender();
    const cli = findCli(undefined, backendDir ?? undefined);
    const imdiskPath = await findImdisk();
    const imdisk: ProbeResult['imdisk'] = imdiskPath
      ? {ok: true, path: imdiskPath}
      : {ok: false, message: IMDISK_MISSING};
    let ramdisk: ProbeResult['ramdisk'] = {ok: false, message: `未找到卷标 ${VOLUME_LABEL} 的 RAM 盘`};
    if (imdiskPath) {
      try {
        const discovered = await discoverRamdisk(this.ramLetter ?? undefined);
        if (discovered.ok) {
          const disk = discovered.value;
          ramdisk = {
            ok: true,
            letter: disk.letter,
            root: disk.root,
            label: disk.label,
            filesystem: disk.filesystem,
            totalBytes: disk.totalBytes || undefined,
            freeBytes: disk.freeBytes || undefined,
          };
          this.ramLetter = disk.letter;
        } else {
          ramdisk = {ok: false, message: userErrorMessage(discovered.error)};
        }
      } catch (err) {
        ramdisk = {ok: false, message: userErrorMessage(err instanceof Error ? err.message : `未找到 ${VOLUME_LABEL}`)};
      }
    }

    return {
      imdisk,
      ramdisk,
      blender: blender ? {ok: true, path: blender} : {ok: false},
      cli: cli ? {ok: true, path: cli} : {ok: false},
    };
  }

  async inspect(blend: string): Promise<{ok: boolean; scene?: SceneState; error?: string}> {
    this.emit({type: 'inspect_start', blend});
    const blender = findBlender();
    if (!blender) {
      const error = '找不到 blender.exe';
      this.emit({type: 'inspect_error', error});
      return {ok: false, error};
    }
    if (!blend || !existsFile(blend)) {
      const error = '请选择 .blend 文件';
      this.emit({type: 'inspect_error', error});
      return {ok: false, error};
    }
    const outDir = logRoot();
    fs.mkdirSync(outDir, {recursive: true});
    const jsonOut = path.join(outDir, `inspect-${process.pid}-${Date.now()}.json`);
    const inspectPy = path.join(backendRoot(), 'pipeline', 'inspect_blend.py');
    try {
      const result = await runCommand(
        blender,
        ['-b', blend, '--python-exit-code', '1', '--python', inspectPy, '--', '--json-out', jsonOut],
        240000
      );
      let scene: SceneState | undefined;
      if (existsFile(jsonOut)) {
        try {
          scene = JSON.parse(fs.readFileSync(jsonOut, 'utf8')) as SceneState;
        } catch (err) {
          const error = `inspect JSON 损坏: ${err instanceof Error ? err.message : String(err)}`;
          this.emit({type: 'inspect_error', error});
          return {ok: false, error};
        }
        try {
          fs.unlinkSync(jsonOut);
        } catch {
          /* ignore */
        }
      }
      if (!scene || scene.ok === false) {
        const error =
          (scene && scene.error) ||
          result.stderr.trim() ||
          result.stdout.trim().slice(-800) ||
          `读取场景失败 (exit ${result.code})`;
        this.emit({type: 'inspect_error', error});
        return {ok: false, error};
      }
      this.emit({type: 'inspect_done', ...scene});
      return {ok: true, scene};
    } catch (err) {
      const error = err instanceof Error ? err.message : String(err);
      this.emit({type: 'inspect_error', error});
      return {ok: false, error};
    }
  }

  async start(req: ExportRequest): Promise<{ok: boolean; error?: string}> {
    if (this.running) {
      return {ok: false, error: '已有任务在跑'};
    }
    if (req.mode !== 'sr' && req.mode !== 'rr') {
      return {ok: false, error: req.mode === ('nr' as string) ? NR_RESERVED : '未知模式'};
    }
    const fg = parseFg(req.fg);
    if (!req.blend || !existsFile(req.blend)) {
      return {ok: false, error: '请选择 .blend 文件'};
    }
    const outputTemplate = (req.outputTemplate || '').trim();
    if (!outputTemplate) {
      return {ok: false, error: '文件名模板无法解析'};
    }
    const blender = findBlender(req.blender);
    const cli = findCli(req.cli, backendDir ?? undefined);
    if (!blender) return {ok: false, error: '找不到 blender.exe'};
    if (!cli) return {ok: false, error: '找不到 dlss-compositor-cli.exe'};

    const exportDir = path.resolve(req.exportDir);
    try {
      ensureDir(exportDir);
    } catch (err) {
      const code = err && typeof err === 'object' && 'code' in err ? String((err as {code?: string}).code) : '';
      if (code === 'EPERM' || code === 'EACCES') {
        return {ok: false, error: `无法创建导出目录 ${exportDir}（不能选盘符根目录，请选一个子文件夹）`};
      }
      throw err;
    }

    // Input buffer: a RAM disk (auto-created in packaged builds when missing) or
    // any user-chosen cache directory — the CLI just receives a path.
    const rpc = await this.ensureCli(cli, exportDir);
    let ramDir: string;
    if (req.useRamdisk) {
      const prepared = await this.prepareRamdisk(
        Math.min(64, Math.max(1, Math.round(req.ramdiskGb ?? 2)))
      );
      if (!prepared.ok) return {ok: false, error: prepared.error};
      const letter = (prepared.letter ?? 'R').replace(':', '');
      this.ramLetter = letter;
      ramDir = path.join(`${letter}:\\`, 'dlss');
      ensureDir(ramDir);
      this.ramDir = ramDir;
      const probe = await this.probe();
      if (!probe.ramdisk.ok) {
        const message = userErrorMessage(
          probe.imdisk.ok
            ? probe.ramdisk.message || `未找到 ${VOLUME_LABEL}`
            : probe.imdisk.message || IMDISK_MISSING,
          String(probe.ramdisk.letter || letter)
        );
        return {ok: false, error: message};
      }
      if (path.parse(exportDir).root.toUpperCase().startsWith(`${letter}:`)) {
        return {ok: false, error: '导出目录不能写在内存盘上'};
      }
    } else {
      const cacheRoot = path.resolve((req.cachePath || '').trim());
      if (!cacheRoot || cacheRoot === path.parse(cacheRoot).root.replace(/\\$/, '')) {
        return {ok: false, error: '请先在 高级 → 缓存路径 里选择缓存目录'};
      }
      this.ramdiskManaged = false;
      this.lastRamdiskBytes = 0;
      ramDir = path.join(cacheRoot, 'dlss-cache');
      ensureDir(ramDir);
      this.ramDir = ramDir;
    }
    for (const name of fs.readdirSync(ramDir)) {
      const lower = name.toLowerCase();
      if (lower.endsWith('.exr') || lower.startsWith('dlssconfig') || lower.startsWith('preview_')) {
        try {
          fs.unlinkSync(path.join(ramDir, name));
        } catch {
          /* ignore */
        }
      }
    }

    this.abandonBlender();
    this.session += 1;
    const session = this.session;
    this.pollInFlight = false;
    this.exportSettled = false;
    this.stopping = false;   // a prior drain-in-progress must not kill this job
    this.stopDeadline = 0;
    this.running = true;
    this.emit({
      type: 'export_start',
      mode: req.mode,
      blend: req.blend,
      ramDir,
      exportDir,
      outputTemplate,
      fg,
      willUnmount: false,
    });
    debug(`start blender=${blender} cli=${cli} ram=${ramDir} export=${exportDir} template=${outputTemplate}`);

    try {
      let ngx: Record<string, unknown> = {};
      try {
        ngx = await rpc.request('getStatus', {}, 15000);
        this.emit({type: 'ngx_status', ...ngx, cli});
      } catch (err) {
        debug(`getStatus: ${err instanceof Error ? err.message : String(err)}`);
      }
      const native = Boolean(req.nativeResBuffers);
      let pct = req.effectivePercentage ?? mergeOverrides(req).percentage ?? req.percentage ?? 100;
      if (native && req.mode === 'rr') {
        pct = 100;
      } else if (req.mode === 'rr') {
        const rrQuality = req.quality.toUpperCase();
        if (!RR_QUALITY_IDS.has(rrQuality)) {
          throw new Error(
            `RR 不支持质量档 ${req.quality}（NGX RR 仅提供 DLAA/高质量/平衡/性能），请回导出页重选`,
          );
        }
        pct = RR_QUALITY_PERCENT[rrQuality] ?? 58;
      }
      const dual = native && req.mode === 'sr' && pct < 100;
      const colorDir = path.join(ramDir, 'color');
      const guidesDir = path.join(ramDir, 'guides');
      if (dual) {
        fs.mkdirSync(colorDir, {recursive: true});
        fs.mkdirSync(guidesDir, {recursive: true});
      }
      // dlssnrbuffer goes into a per-video subfolder `<video>_dlssbuffer`
      // (template minus the frame token/extension) so exports don't mix
      // guidance buffers — the NR/FG tool auto-suggests the same name.
      let nrBufferDir: string | undefined;
      if (req.nrBuffer) {
        const baseName = outputTemplate
          .replace(/\{frame[^}]*\}/gi, '')
          .replace(/\.(mov|mp4|m4v|mkv|avi|png|exr)$/i, '')
          .replace(/[._\s]+$/, '')
          .trim();
        nrBufferDir = path.join(exportDir, `${baseName || 'dlssnrbuffer'}_dlssbuffer`);
        fs.mkdirSync(nrBufferDir, {recursive: true});
        this.nrBufferDir = nrBufferDir;
      } else {
        this.nrBufferDir = null;
      }
      // Unified export bar, phase 1 of 3: prep (写 dlssconfig/LUT) owns the
      // first 10%. percent stays 0 until inject.py's first bake_progress event
      // arrives — the GUI shows an indeterminate bar until then.
      this.emit({type: 'engine_progress', percent: 0, phase: 'prepare'});
      await this.bakeConfig(blender, req, ramDir, pct);
      if (!existsFile(path.join(ramDir, 'dlssconfig.json'))) {
        throw new Error('inject 未写出 dlssconfig.json');
      }

      // 分段并行：N 个 Blender/CLI 实例各跑一段（dlssconfig 由 bake 写全量，
      // 段间共享；jitter 按绝对帧号索引，天然对齐）。串行主流程继续走下方。
      const segCount = Math.max(0, Math.trunc(req.parallelSegments ?? 0));
      if (segCount >= 2) {
        if (native) {
          throw new Error('分段并行不支持原生分辨率缓冲，请先关闭该选项');
        }
        await this.startSegments(req, blender, cli, ramDir, exportDir, outputTemplate, fg, pct, segCount, session);
        return {ok: true};
      }

      const jobParams: Record<string, unknown> = {
        mode: req.mode,
        input_dir: ramDir,
        output_dir: exportDir,
        output_format: req.outputFormat,
        output_template: outputTemplate,
        frame_start: req.frameStart,
        frame_end: req.frameEnd,
        window: req.window ?? 6,
        watch: true,
        wait_for_appearance: true,
        delete_consumed: true,
        native_res_buffers: native,
        nr_buffer: Boolean(req.nrBuffer),
        nr_buffer_dir: nrBufferDir,
        color_dir: dual ? colorDir : undefined,
        guides_dir: dual ? guidesDir : undefined,
        config: path.join(ramDir, 'dlssconfig.json'),
        rr_preset: 'F',
        overwrite_mode: 'overwrite',
      };
      if (fg >= 2) jobParams.fg_multiplier = fg;
      const job = await this.startJob(rpc, jobParams);
      debug(`start_job ${JSON.stringify(job)}`);
      this.emit({type: 'job_started', ...job, native_res_buffers: native, dual_pass: dual, fg});

      this.pollTimer = setInterval(() => {
        void this.pollOnce(session);
      }, 400);

      if (native) {
        void this.runNativeBlenderLoop(
          session,
          blender,
          req,
          ramDir,
          colorDir,
          guidesDir,
          dual,
          pct,
          this.lastRamdiskBytes
        );
      } else {
        this.spawnBlender(
          blender,
          this.buildInjectArgs(req, ramDir, 'combined', pct, {
            frameStart: req.frameStart,
            frameEnd: req.frameEnd,
            skipConfig: true,
            configDir: ramDir,
          })
        );
      }
      return {ok: true};
    } catch (err) {
      const message = userErrorMessage(err instanceof Error ? err.message : String(err));
      debug(`start failed: ${message}`);
      this.running = false;
      this.abandonBlender();
      if (this.stopping) {
        // User stop during bake: killing blender surfaces here as a bake
        // failure — stop() already emitted 'stopped', don't stack an error.
        return {ok: false, error: message};
      }
      this.emit({type: 'error', message});
      return {ok: false, error: message};
    }
  }

  async stop(): Promise<{ok: boolean; unmounted: false}> {
    const hadWork = this.running || this.blender != null || this.pollTimer != null || this.segTimer != null;
    this.abandonBlender();
    let warning: string | undefined;
    if (this.segs.length > 0) {
      // 分段并行：请求每个存活段停止、杀各段 Blender，轮询继续等各 CLI
      // 收尾（writer drain + mov trailer）后再落 'stopped'。
      await Promise.all(this.segs.map(async (seg) => {
        killTree(seg.blender);
        if (seg.rpc?.writable()) {
          try {
            await seg.rpc.request('stop', {}, 5000);
          } catch (err) {
            debug(`seg${seg.index} stop: ${err instanceof Error ? err.message : String(err)}`);
          }
        }
      }));
    } else if (this.rpc?.writable()) {
      try {
        await this.rpc.request('stop', {}, 5000);
      } catch (err) {
        warning = err instanceof Error ? err.message : String(err);
        if (warning.includes('write after end')) warning = 'RPC 连接已关闭';
        debug(`stop rpc: ${warning}`);
      }
    } else if (this.cli && !this.rpc?.writable()) {
      warning = 'RPC 连接已关闭';
    }
    this.running = false;
    if (hadWork && this.pollTimer) {
      // Keep the poll alive through the writer drain: the bar keeps moving
      // (finalize slice) and 'stopped' fires once the mov trailer actually
      // landed. pollOnce owns the settle; 60s deadline guards a dead CLI.
      this.stopping = true;
      this.stopDeadline = Date.now() + 60000;
    } else if (hadWork && this.segTimer) {
      this.stopping = true;
      this.segStopDeadline = Date.now() + 60000;
    } else {
      // Nothing polling (bake phase / idle): settle immediately. Bump the
      // session so the in-flight start() flow stops emitting events.
      this.session += 1;
      this.stopping = false;
      if (hadWork) {
        this.discardSegs();
        this.cleanupSegs();
        this.emit({type: 'stopped', ramdiskUnmounted: false, letter: this.ramLetter, warning});
        void this.sweepRamDir();
      }
    }
    return {ok: true, unmounted: false};
  }

  // ── NR tool (standalone DLSSNR pass over its own CLI process) ──────────────

  private nrAlive(): boolean {
    return Boolean(this.nrCli && this.nrCli.exitCode === null && this.nrRpc?.writable());
  }

  /** Spawn the tool's dedicated CLI (lazy, one per app session). */
  private async ensureNrCli(cli: string): Promise<StdioRpcClient> {
    if (this.nrAlive() && this.nrCliPath === cli && this.nrRpc) {
      return this.nrRpc;
    }
    this.discardNrCli();
    const logs = logRoot();
    fs.mkdirSync(logs, {recursive: true});
    const stderrPath = path.join(logs, 'cli.stderr.log');
    if (this.cliLog) {
      this.nrStderrFd = fs.openSync(stderrPath, 'w');
      this.nrCli = spawn(cli, ['--rpc-stdio'], {
        cwd: path.dirname(cli),
        stdio: ['pipe', 'pipe', this.nrStderrFd],
        windowsHide: true,
        env: {
          ...process.env,
          // Release CLI defaults logs to off; keep stderr useful for the tool.
          DLSS_COMPOSITOR_LOG: process.env.DLSS_COMPOSITOR_LOG || 'info',
        },
      });
      this.emit({type: 'nr_cli_pid', pid: this.nrCli.pid, stderr: stderrPath});
    } else {
      this.nrCli = spawn(cli, ['--rpc-stdio'], {
        cwd: path.dirname(cli),
        stdio: ['pipe', 'pipe', 'ignore'],
        windowsHide: true,
        env: process.env,
      });
      this.emit({type: 'nr_cli_pid', pid: this.nrCli.pid});
    }
    this.nrCliPath = cli;
    const child = this.nrCli;
    child.on('error', (err) => debug(`nr cli spawn error: ${err.message}`));
    child.on('exit', (code, signal) => {
      debug(`nr cli exit code=${code} signal=${signal}`);
      if (this.nrCli === child) {
        this.nrRpc = null;
        this.nrCli = null;
        this.nrCliPath = null;
      }
    });
    this.nrRpc = new StdioRpcClient(this.nrCli);
    debug(`nr cli pid=${this.nrCli.pid}`);
    return this.nrRpc;
  }

  private discardNrCli(): void {
    try {
      this.nrRpc?.close();
    } catch {
      /* ignore */
    }
    this.nrRpc = null;
    killTree(this.nrCli);
    this.nrCli = null;
    this.nrCliPath = null;
    if (this.nrStderrFd != null) {
      try {
        fs.closeSync(this.nrStderrFd);
      } catch {
        /* ignore */
      }
      this.nrStderrFd = null;
    }
  }

  /** Open an NR session over a color source, optionally a dlssnrbuffer
   *  guidance dir (no buffer = guidance mode 无 only). Boots the tool's
   *  dedicated CLI on demand (the NR tool works without an open project). */
  async nrOpen(params: {
    color_source: string;
    buffer_dir?: string;
  }): Promise<Record<string, unknown>> {
    const cli = findCli(undefined, backendDir ?? undefined);
    if (!cli) return {ok: false, error: '找不到 dlss-compositor-cli.exe'};
    const rpc = await this.ensureNrCli(cli);
    return rpc.request('nrtool_open', params, 120000);
  }

  async nrPreview(
    frame: number,
    settings: Record<string, unknown>
  ): Promise<Record<string, unknown>> {
    if (!this.nrRpc?.writable()) return {ok: false, error: 'NR 引擎未启动（先打开媒体）'};
    return this.nrRpc.request('nrtool_preview', {frame, ...settings}, 120000);
  }

  /** Start an NR export and poll progress until it settles, forwarding
   *  nrtool_progress / nrtool_export_done / nrtool_export_error events. */
  async nrExport(params: Record<string, unknown>): Promise<Record<string, unknown>> {
    if (!this.nrRpc?.writable()) return {ok: false, error: 'NR 引擎未启动（先打开媒体）'};
    this.nrStopRequested = false;
    const result = await this.nrRpc.request('nrtool_export', params, 30000);
    if (this.nrPollTimer) clearInterval(this.nrPollTimer);
    this.nrPollInFlight = false;
    this.nrPollTimer = setInterval(() => void this.nrPollOnce(), 400);
    return result;
  }

  async nrClose(): Promise<Record<string, unknown>> {
    if (this.nrPollTimer) {
      clearInterval(this.nrPollTimer);
      this.nrPollTimer = null;
    }
    if (!this.nrRpc?.writable()) return {ok: true};
    return this.nrRpc.request('nrtool_close', {}, 30000);
  }

  /** Cancel a running NR export via the tool process's stop RPC. */
  async nrStop(): Promise<Record<string, unknown>> {
    this.nrStopRequested = true;
    if (!this.nrRpc?.writable()) return {ok: true};
    return this.nrRpc.request('stop', {}, 5000);
  }

  private async nrPollOnce(): Promise<void> {
    if (!this.nrRpc?.writable() || this.nrPollInFlight) return;
    this.nrPollInFlight = true;
    try {
      const progress = await this.nrRpc.request('progress', {}, 5000);
      const state = progress.state;
      this.emit({type: 'nrtool_progress', ...progress});
      if (state === 2 || state === 'done') {
        this.stopNrPoll();
        this.nrStopRequested = false;
        this.emit({type: 'nrtool_export_done'});
      } else if (state === 3 || state === 'error') {
        this.stopNrPoll();
        // The CLI reports a user stop as state 3 as well; disambiguate via
        // the stop flag so the UI shows 已停止 instead of a failure.
        const stopped = this.nrStopRequested;
        this.nrStopRequested = false;
        this.emit(
          stopped
            ? {type: 'nrtool_export_stopped'}
            : this.cliLog
              ? {type: 'nrtool_export_error', message: 'NR 导出失败（详见 cli.stderr.log）'}
              : {type: 'nrtool_export_error', message: 'NR 导出失败'}
        );
      }
    } catch (err) {
      this.emit({type: 'rpc_warn', message: err instanceof Error ? err.message : String(err)});
    } finally {
      this.nrPollInFlight = false;
    }
  }

  private stopNrPoll(): void {
    if (this.nrPollTimer) {
      clearInterval(this.nrPollTimer);
      this.nrPollTimer = null;
    }
  }

  /** Copy the per-frame camera data next to the guidance buffer — the NR/FG
   *  tool needs it for FG. Called when a job settles (success) and when the
   *  user stops one (a stopped export must stay FG-capable too). */
  private persistDlssconfig(): void {
    if (!this.nrBufferDir || !this.ramDir) return;
    try {
      const cfgSrc = path.join(this.ramDir, 'dlssconfig.json');
      if (existsFile(cfgSrc)) {
        fs.copyFileSync(cfgSrc, path.join(this.nrBufferDir, 'dlssconfig.json'));
      }
    } catch (err) {
      debug(`dlssconfig copy into buffer dir failed: ${String(err)}`);
    }
  }

  /**
   * Delete leftover input frames once a job settles. Anything the pipeline did not
   * consume (failed decodes, tail frames written after the engine stopped) is
   * garbage; on a fixed-size RAM disk it must not sit there until the next run.
   */
  private async sweepRamDir(): Promise<void> {
    const dir = this.ramDir;
    if (!dir) return;
    try {
      const names = await fs.promises.readdir(dir);
      const stale = names.filter((name) => /\.exr$/i.test(name) || /^preview_/i.test(name));
      await Promise.all(
        stale.map((name) => fs.promises.rm(path.join(dir, name), {force: true}).catch(() => {}))
      );
      if (stale.length) debug(`swept ${stale.length} stale file(s) from ${dir}`);
    } catch {
      /* ram dir already gone */
    }
  }

  /** Idempotent teardown: stop jobs + CLI, then unmount a RAM disk this
   *  process auto-created. The promise is cached so window-all-closed and
   *  before-quit (and any later caller) share one teardown run. */
  private shutdownPromise: Promise<void> | null = null;

  shutdown(): Promise<void> {
    if (!this.shutdownPromise) {
      this.shutdownPromise = (async () => {
        this.stopNrPoll();
        if (this.segTimer) {
          clearInterval(this.segTimer);
          this.segTimer = null;
        }
        await this.stop();
        // Kill both CLIs before unmounting: their RAM-disk watcher/file
        // handles would make the graceful imdisk detach fail.
        this.discardNrCli();
        this.discardSegs();
        this.discardCli();
        await this.releaseRamdisk();
      })();
    }
    return this.shutdownPromise;
  }

  /** Find an existing DLSSRAM disk; in packaged builds create one when missing
   *  (imdisk needs an elevated token). Dev builds never create. */
  private async prepareRamdisk(sizeGb: number): Promise<{ok: boolean; letter?: string; error?: string}> {
    try {
      const discovered = await discoverRamdisk(this.ramLetter ?? undefined);
      if (discovered.ok) {
        const letter = discovered.value.letter;
        this.ramdiskManaged = false;
        debug(`ramdisk: reusing existing ${letter}:`);
        return {ok: true, letter};
      }
    } catch (err) {
      debug(`ramdisk discover: ${err instanceof Error ? err.message : String(err)}`);
    }
    if (!this.autoRamdisk) {
      return {
        ok: false,
        error: '未找到内存盘（DLSSRAM）。开发环境请用 ImDisk Toolkit 手动挂载，或改用导出页的缓存路径模式。',
      };
    }
    const created = await createRamdisk(`${sizeGb}G`);
    if (created.ok) {
      const letter = created.value.letter;
      this.ramdiskManaged = true;
      debug(`ramdisk: created ${letter}: (${created.value.filesystem})`);
      return {ok: true, letter};
    }
    return {ok: false, error: userErrorMessage(created.error)};
  }

  private async releaseRamdisk(): Promise<void> {
    if (!this.ramdiskManaged || !this.ramLetter) return;
    try {
      const released = await unmountRamdisk(this.ramLetter);
      if (released.ok) debug(`ramdisk ${this.ramLetter}: unmounted`);
      else debug(`ramdisk unmount: ${released.error}`);
    } catch (err) {
      debug(`ramdisk unmount: ${err instanceof Error ? err.message : String(err)}`);
    }
    this.ramdiskManaged = false;
  }

  private blenderLineBuf = '';

  private buildInjectArgs(
    req: ExportRequest,
    outputDir: string,
    bufferPass: 'combined' | 'beauty' | 'guides',
    percentage: number | undefined,
    extra?: {
      frameStart: number;
      frameEnd: number;
      skipConfig?: boolean;
      dryRun?: boolean;
      configDir?: string;
    }
  ): string[] {
    const inject = path.join(backendRoot(), 'pipeline', 'inject.py');
    const args = [
      '-b',
      req.blend,
      '--python',
      inject,
      '--',
      '--output-dir',
      outputDir,
      '--quality',
      req.quality,
      '--mode',
      req.mode,
    ];
    const ov = {...mergeOverrides(req)};
    if (extra) {
      ov.frameStart = extra.frameStart;
      ov.frameEnd = extra.frameEnd;
    }
    if (percentage != null) ov.percentage = percentage;
    if (bufferPass === 'guides') {
      ov.percentage = 100;
      ov.samples = 1;
      ov.useAdaptiveSampling = false;
      ov.useDenoising = false;
    }
    pushOverrideArgs(args, ov);
  // Perf fallbacks: renderer sends explicit values; inject.py applies the
  // same defaults when the args are absent (direct CLI use).
  if (req.perfPersistent != null) {
    args.push('--perf-persistent', req.perfPersistent ? 'true' : 'false');
  }
  if (req.perfCameraCull != null) {
    args.push('--perf-camera-cull', req.perfCameraCull ? 'true' : 'false');
  }
  if (req.cameraCullMargin != null) args.push('--camera-cull-margin', String(req.cameraCullMargin));
  if (req.perfDistanceCull != null) {
    args.push('--perf-distance-cull', req.perfDistanceCull ? 'true' : 'false');
  }
  if (req.distanceCullMargin != null) {
    args.push('--distance-cull-margin', String(req.distanceCullMargin));
  }
  if (req.perfTileSize != null) args.push('--tile-size', String(req.perfTileSize));
    if (req.mode === 'rr') {
      // DLSS-RR §3.5: white-noise-like sampling; BLUE_NOISE correlates pixels and
      // is tuned for Blender's own temporal accumulation.
      args.push('--engine', 'CYCLES', '--sampling-pattern', 'TABULATED_SOBOL');
    }
    if (req.outputWidth) args.push('--output-width', String(req.outputWidth));
    if (req.outputHeight) args.push('--output-height', String(req.outputHeight));
    if (bufferPass !== 'combined') args.push('--pass', bufferPass);
    if (extra?.dryRun) args.push('--dry-run');
    if (extra?.skipConfig) args.push('--skip-config');
    if (extra?.configDir) args.push('--config-dir', extra.configDir);
    if (extra?.frameStart != null) args.push('--config-frame-start', String(extra.frameStart));
    if (extra?.frameEnd != null) args.push('--config-frame-end', String(extra.frameEnd));
    return args;
  }

  private spawnBlender(blender: string, blenderArgs: string[]): ChildProcess {
    debug(`inject ${JSON.stringify(blenderArgs)}`);
    this.abandonBlender();
    this.blender = spawn(blender, blenderArgs, {
      cwd: backendRoot(),
      windowsHide: true,
    });
    this.emit({type: 'blender_pid', pid: this.blender.pid, cmd: blenderArgs});
    this.blender.stdout?.on('data', (chunk: Buffer) => this.onBlenderChunk(chunk.toString('utf8')));
    this.blender.stderr?.on('data', (chunk: Buffer) => this.onBlenderChunk(chunk.toString('utf8')));
    this.blender.on('close', (code) => {
      this.emit({type: 'blender_exit', code});
    });
    return this.blender;
  }

  private async bakeConfig(
    blender: string,
    req: ExportRequest,
    ramDir: string,
    percentage: number
  ): Promise<void> {
    const args = this.buildInjectArgs(req, ramDir, 'combined', percentage, {
      frameStart: req.frameStart,
      frameEnd: req.frameEnd,
      dryRun: true,
      configDir: ramDir,
    });
    debug(`bake ${JSON.stringify(args)}`);
    const proc = spawn(blender, args, {cwd: backendRoot(), windowsHide: true});
    proc.stdout?.on('data', (chunk: Buffer) => this.onBlenderChunk(chunk.toString('utf8')));
    proc.stderr?.on('data', (chunk: Buffer) => this.onBlenderChunk(chunk.toString('utf8')));
    this.emit({type: 'blender_start', message: '写 dlssconfig / LUT，不渲染', cmd: args});
    const code = await this.waitBlender(proc);
    this.emit({type: 'blender_exit', code, phase: 'config'});
    if (code !== 0) {
      throw new Error(`bake dlssconfig 失败（${code}）`);
    }
  }

  private waitBlender(proc: ChildProcess): Promise<number> {
    return new Promise((resolve) => {
      if (proc.exitCode != null) {
        resolve(proc.exitCode);
        return;
      }
      proc.once('close', (code) => resolve(code ?? 1));
    });
  }

  private async runNativeBlenderLoop(
    session: number,
    blender: string,
    req: ExportRequest,
    ramDir: string,
    colorDir: string,
    guidesDir: string,
    dual: boolean,
    pct: number,
    totalBytes: number
  ): Promise<void> {
    const outW = req.outputWidth ?? 1920;
    const outH = req.outputHeight ?? 1080;
    const renderW = Math.max(1, Math.floor((outW * pct) / 100));
    const renderH = Math.max(1, Math.floor((outH * pct) / 100));
    const diskBytes = totalBytes > 0 ? totalBytes : 8 * 1024 ** 3;
    const budget = ramWindowBudget(diskBytes);
    const perFrame = estimateWindowFrameBytes(renderW, renderH, outW, outH, dual);
    const windows = planRamWindows(
      req.frameStart,
      req.frameEnd,
      perFrame,
      budget,
      req.ramWindowFrames ?? null
    );
    this.emit({
      type: 'ram_window_plan',
      message: dual
        ? `原生缓冲：${windows.length} 个窗口，每窗 Beauty+Guides`
        : `原生缓冲：一趟输出分辨率，${windows.length} 个窗口`,
      windows: windows.length,
      dual_pass: dual,
      ram_total_bytes: diskBytes,
      ram_budget_bytes: budget,
      estimated_exr_bytes: perFrame,
    });
    let wroteConfig = true;
    for (let i = 0; i < windows.length; i++) {
      if (session !== this.session || !this.running) return;
      const [winStart, winEnd] = windows[i];
      const extra = {
        frameStart: winStart,
        frameEnd: winEnd,
        skipConfig: wroteConfig,
        configDir: ramDir,
      };
      if (dual) {
        this.emit({
          type: 'job_phase',
          message: phaseLabel('beauty', winStart, winEnd),
          phase: 'beauty',
          frame_start: winStart,
          frame_end: winEnd,
        });
        const beauty = this.spawnBlender(
          blender,
          this.buildInjectArgs(req, colorDir, 'beauty', pct, extra)
        );
        const beautyCode = await this.waitBlender(beauty);
        if (beautyCode !== 0) {
          this.emit({type: 'error', message: 'Beauty 趟失败'});
          return;
        }
        wroteConfig = true;
        if (session !== this.session || !this.running) return;
        this.emit({
          type: 'job_phase',
          message: phaseLabel('guides', winStart, winEnd),
          phase: 'guides',
          frame_start: winStart,
          frame_end: winEnd,
        });
        extra.skipConfig = true;
        const guides = this.spawnBlender(
          blender,
          this.buildInjectArgs(req, guidesDir, 'guides', 100, extra)
        );
        const guidesCode = await this.waitBlender(guides);
        if (guidesCode !== 0) {
          this.emit({type: 'error', message: 'Guides 趟失败'});
          return;
        }
      } else {
        this.emit({
          type: 'job_phase',
          message: phaseLabel('combined', winStart, winEnd),
          phase: 'combined',
          frame_start: winStart,
          frame_end: winEnd,
        });
        const proc = this.spawnBlender(
          blender,
          this.buildInjectArgs(req, ramDir, 'combined', pct, extra)
        );
        const code = await this.waitBlender(proc);
        if (code !== 0) {
          this.emit({type: 'error', message: `Blender 失败（${code}）`});
          return;
        }
        wroteConfig = true;
      }
      this.emit({
        type: 'job_phase',
        message: phaseLabel('rebuild', winStart, winEnd),
        phase: 'rebuild',
        frame_start: winStart,
        frame_end: winEnd,
      });
    }
  }

  // ── Segmented parallel rendering ─────────────────────────────────────────────
  //
  // Splits [frameStart, frameEnd] into N contiguous slices and runs one
  // Blender + one CLI per slice concurrently. The per-frame "Initializing"
  // depsgraph sync is CPU-bound and leaves the GPU idle; N instances overlap
  // sync-of-A with render-of-B. Each later slice renders `warmup` frames
  // before its output range so NGX history converges before the first emitted
  // frame (the CLI's output_frame_begin holds those writes back), and every
  // slice continues the shared output numbering via output_frame_start_index —
  // merged results match one continuous run exactly. Sequence formats land in
  // the export dir directly (absolute numbering, no merge); ProRes/H265
  // segments are stream-copy concatenated via mov_concat, then deleted.

  private async startSegments(
    req: ExportRequest,
    blenderExe: string,
    cliPath: string,
    ramDir: string,
    exportDir: string,
    outputTemplate: string,
    fg: number,
    pct: number,
    segCount: number,
    session: number
  ): Promise<void> {
    const warmupPref = Math.max(0, Math.trunc(req.parallelWarmup ?? 16));
    const total = req.frameEnd - req.frameStart + 1;
    const count = Math.max(2, Math.min(segCount, total));
    const fgMult = fg >= 2 ? fg : 1;
    const isVideo = req.outputFormat === 'prores_422hq' || req.outputFormat === 'h265_hevc';
    const segDirs: string[] = [];

    try {
      this.discardSegs();
      // 段缓冲目录 + 段视频残留清理（上次崩溃可能留下旧 EXR / 旧段 MOV）。
      const segDirs: string[] = [];
      for (let i = 0; i < count; i++) {
        const dir = path.join(ramDir, `seg${i}`);
        fs.rmSync(dir, {recursive: true, force: true});
        fs.mkdirSync(dir, {recursive: true});
        segDirs.push(dir);
      }
      let finalVideoPath: string | null = null;
      if (isVideo) {
        const dot = outputTemplate.lastIndexOf('.');
        const stem = dot > 0 ? outputTemplate.slice(0, dot) : outputTemplate;
        const ext = dot > 0 ? outputTemplate.slice(dot) : '';
        finalVideoPath = path.join(exportDir, outputTemplate);
        this.segFinalVideoPath = finalVideoPath;
        try {
          for (const name of fs.readdirSync(exportDir)) {
            if (name.startsWith(`${stem}__seg`) && name.toLowerCase().endsWith(ext.toLowerCase())) {
              fs.rmSync(path.join(exportDir, name), {force: true});
            }
          }
        } catch {
          /* export dir scan best-effort; writers overwrite anyway */
        }
      }

      // 均分段：rem 个段多一帧。renderStart 往前借 warmup 帧喂 NGX 历史
      // （clamp 到任务起点；第 0 段就是连续渲染的真实起点，无需预热）。
      const base = Math.floor(total / count);
      const rem = total % count;
      let cursor = req.frameStart;
      const plan: Array<{segStart: number; segEnd: number; renderStart: number; expected: number; offset: number}> = [];
      for (let i = 0; i < count; i++) {
        const size = base + (i < rem ? 1 : 0);
        const segStart = cursor;
        const segEnd = cursor + size - 1;
        cursor += size;
        const renderStart = i === 0
          ? segStart
          : Math.max(req.frameStart, segStart - Math.min(warmupPref, segStart - req.frameStart));
        const warmupHere = segStart - renderStart;
        const expected = fgMult >= 2
          ? (warmupHere > 0 ? fgMult * size : fgMult * size - (fgMult - 1))
          : size;
        const offset = 1 + (segStart - req.frameStart) * fgMult;
        plan.push({segStart, segEnd, renderStart, expected, offset});
      }
      this.segOutputsTotal = plan.reduce((sum, p) => sum + p.expected, 0);
      this.segJobRange = {start: req.frameStart, end: req.frameEnd, totalInputs: total};
      this.segLastOutputs = -1;
      this.segLastSyntheticFrame = 0;
      this.segLastProgressAt = Date.now();

      // x 个 CLI 进程（NGX 上下文彼此独立，历史不跨界）。
      const spawned: SegRuntime[] = [];
      for (let i = 0; i < count; i++) {
        const p = plan[i];
        const segVideoPath = isVideo ? segVideoName(exportDir, outputTemplate, i) : null;
        const seg: SegRuntime = {
          index: i,
          cli: null,
          rpc: null,
          blender: null,
          stderrFd: null,
          bufferDir: segDirs[i],
          segVideoPath,
          segStart: p.segStart,
          segEnd: p.segEnd,
          renderStart: p.renderStart,
          outputOffset: p.offset - 1,
          expectedOutputs: p.expected,
          outputsDone: 0,
          done: false,
          failed: false,
          cancelled: false,
          errorDetail: '',
        };
        const {cli, rpc, stderrFd} = this.spawnSegCli(cliPath, i, exportDir);
        seg.cli = cli;
        seg.rpc = rpc;
        seg.stderrFd = stderrFd;
        spawned.push(seg);
      }
      this.segs = spawned;

      // x 个 start_job（窗口 + 输出门 + 续号），共享 ramDir 根的 dlssconfig。
      for (let i = 0; i < this.segs.length; i++) {
        const seg = this.segs[i];
        const p = plan[i];
        const jobParams: Record<string, unknown> = {
          mode: req.mode,
          input_dir: seg.bufferDir,
          output_dir: exportDir,
          output_format: req.outputFormat,
          output_template: seg.segVideoPath ? path.basename(seg.segVideoPath) : outputTemplate,
          frame_start: seg.segStart,
          frame_end: seg.segEnd,
          input_frame_begin: seg.renderStart,
          input_frame_end: seg.segEnd,
          output_frame_begin: seg.segStart,
          output_frame_start_index: p.offset,
          window: req.window ?? 6,
          watch: true,
          wait_for_appearance: true,
          delete_consumed: true,
          native_res_buffers: false,
          nr_buffer: Boolean(req.nrBuffer),
          nr_buffer_dir: this.nrBufferDir ?? undefined,
          config: path.join(ramDir, 'dlssconfig.json'),
          rr_preset: 'F',
          overwrite_mode: 'overwrite',
        };
        if (fg >= 2) jobParams.fg_multiplier = fg;
        await this.startJob(seg.rpc!, jobParams);
      }

      // x 个 Blender 注入进程：各自渲染自己的段（含预热帧）。
      for (const seg of this.segs) {
        const args = this.buildInjectArgs(req, seg.bufferDir, 'combined', pct, {
          frameStart: seg.renderStart,
          frameEnd: seg.segEnd,
          skipConfig: true,
          configDir: ramDir,
        });
        this.spawnSegBlender(seg, blenderExe, args);
      }

      debug(`segments: ${this.segs.map((s) =>
        `#${s.index}[${s.renderStart}..${s.segEnd} out@${s.segStart}]`
      ).join(' ')} totalOutputs=${this.segOutputsTotal}`);

      // NGX 状态卡只报一次（每段 CLI 都有自己的上下文，状态一致）。
      try {
        const ngx = await this.segs[0].rpc!.request('getStatus', {}, 15000);
        this.emit({type: 'ngx_status', ...ngx, cli: cliPath});
      } catch (err) {
        debug(`getStatus: ${err instanceof Error ? err.message : String(err)}`);
      }

      this.segTimer = setInterval(() => void this.pollSegments(session), 400);
    } catch (err) {
      // 部分段已起的失败：全部回收，异常继续抛给 start() 的统一错误出口。
      for (const s of this.segs) killTree(s.blender);
      this.discardSegs();
      for (const dir of segDirs) {
        try { fs.rmSync(dir, {recursive: true, force: true}); } catch { /* ignore */ }
      }
      this.segs = [];
      throw err;
    }
  }

  /** Spawn one CLI for a segment (mirrors ensureCli's stderr handling, per seg). */
  private spawnSegCli(cliPath: string, index: number, exportDir: string):
    {cli: ChildProcess; rpc: StdioRpcClient; stderrFd: number | null} {
    let stderrFd: number | null = null;
    let child: ChildProcess;
    if (this.cliLog) {
      const stderrPath = path.join(exportDir, `cli.seg${index}.stderr.log`);
      stderrFd = fs.openSync(stderrPath, 'w');
      child = spawn(cliPath, ['--rpc-stdio'], {
        cwd: path.dirname(cliPath),
        stdio: ['pipe', 'pipe', stderrFd],
        windowsHide: true,
        env: {
          ...process.env,
          DLSS_COMPOSITOR_LOG: process.env.DLSS_COMPOSITOR_LOG || 'info',
        },
      });
      this.emit({type: 'cli_pid', segment: index, pid: child.pid, stderr: stderrPath});
    } else {
      child = spawn(cliPath, ['--rpc-stdio'], {
        cwd: path.dirname(cliPath),
        stdio: ['pipe', 'pipe', 'ignore'],
        windowsHide: true,
        env: process.env,
      });
      this.emit({type: 'cli_pid', segment: index, pid: child.pid});
    }
    const proc = child;
    proc.on('error', (err) => debug(`seg${index} cli spawn error: ${err.message}`));
    proc.on('exit', (code, signal) => debug(`seg${index} cli exit code=${code} signal=${signal}`));
    return {cli: child, rpc: new StdioRpcClient(child), stderrFd};
  }

  private spawnSegBlender(seg: SegRuntime, blenderExe: string, args: string[]): void {
    debug(`seg${seg.index} inject ${JSON.stringify(args)}`);
    seg.blender = spawn(blenderExe, args, {cwd: backendRoot(), windowsHide: true});
    this.emit({type: 'blender_pid', segment: seg.index, pid: seg.blender.pid, cmd: args});
    seg.blender.stdout?.on('data', (chunk: Buffer) => this.onSegBlenderChunk(seg, chunk.toString('utf8')));
    seg.blender.stderr?.on('data', (chunk: Buffer) => this.onSegBlenderChunk(seg, chunk.toString('utf8')));
    seg.blender.on('close', (code) => {
      this.emit({type: 'blender_exit', segment: seg.index, code});
      if (code !== 0 && !seg.done && !seg.failed && !seg.cancelled) {
        seg.failed = true;
        seg.errorDetail = `Blender 退出（${code}）`;
      }
    });
  }

  /** Segment blender lines: forward buffer backpressure, drop per-frame
   *  events (the aggregate CLI progress drives the playhead in parallel mode). */
  private onSegBlenderChunk(seg: SegRuntime, text: string): void {
    let buf = (this.segLineBufs.get(seg.index) ?? '') + text;
    const lines = buf.split(/\r?\n/);
    buf = lines.pop() ?? '';
    this.segLineBufs.set(seg.index, buf);
    for (const line of lines) {
      const stripped = line.trim();
      if (!stripped.startsWith('DLSS_EVENT:')) continue;
      try {
        const payload = JSON.parse(stripped.slice('DLSS_EVENT:'.length)) as Record<string, unknown>;
        const ev = String(payload.event ?? '');
        if (ev === 'progress' || ev === 'frame_done' || ev === 'bake_progress') continue;
        if (ev === 'buffer_full' || ev === 'buffer_recovered') {
          this.emit({type: ev, segment: seg.index, ...payload});
        } else {
          debug(`seg${seg.index} blender event: ${ev}`);
        }
      } catch {
        /* malformed event line — ignore */
      }
    }
  }

  private async pollSegments(session: number): Promise<void> {
    if (session !== this.session || this.segPollInFlight || this.segs.length === 0) return;
    if (this.stopping && Date.now() > this.segStopDeadline) {
      this.finishSegmentsStopped(session, /*force=*/ true);
      return;
    }
    this.segPollInFlight = true;
    try {
      let outputsDone = 0;
      let settledCount = 0;
      for (const seg of this.segs) {
        if (seg.done || seg.failed || seg.cancelled) {
          outputsDone += seg.outputsDone;
          settledCount++;
          continue;
        }
        if (!seg.rpc?.writable()) {
          seg.failed = true;
          seg.errorDetail = 'CLI 进程退出';
          outputsDone += seg.outputsDone;
          settledCount++;
          continue;
        }
        try {
          const progress = await seg.rpc.request('progress', {}, 5000);
          if (session !== this.session) return;
          const state = progress.state;
          if (state === 2 || state === 'done') {
            seg.done = true;
            seg.outputsDone = seg.expectedOutputs;
          } else if (state === 3 || state === 'error') {
            seg.failed = true;
            seg.errorDetail = typeof progress.error === 'string' && progress.error
              ? String(progress.error)
              : 'DLSS 引擎失败';
          } else if (state === 0) {
            seg.cancelled = true;
          } else {
            // CLI current_frame is the absolute output index (segments continue
            // the shared numbering) — subtract earlier segments' outputs.
            const absolute = Number(progress.current_frame) || 0;
            seg.outputsDone = Math.max(
              0, Math.min(seg.expectedOutputs, absolute - seg.outputOffset));
            // 预览只跟第一段：各段输出按共享编号交错落盘，轮流拉取会让
            // 预览在段之间来回跳。
            if (!this.stopping && seg.index === 0) await this.pullSegPreview(seg);
          }
        } catch (err) {
          debug(`seg${seg.index} progress: ${err instanceof Error ? err.message : String(err)}`);
        }
        outputsDone += seg.outputsDone;
        if (seg.done || seg.failed || seg.cancelled) settledCount++;
      }
      if (session !== this.session) return;

      const failedSeg = this.segs.find((s) => s.failed);
      if (failedSeg && !this.stopping) {
        this.finishSegmentsError(session, failedSeg);
        return;
      }

      const anyBlenderAlive = this.segs.some((s) => s.blender && s.blender.exitCode === null);
      if (outputsDone !== this.segLastOutputs || anyBlenderAlive) {
        this.segLastProgressAt = Date.now();
      }
      this.segLastOutputs = outputsDone;
      // 所有 Blender 都退出了但消费进度停摆 2 分钟 → 段缺帧 / CLI 卡死，报错。
      if (!anyBlenderAlive && settledCount < this.segs.length &&
          Date.now() - this.segLastProgressAt > 120000) {
        const stuck = this.segs.find((s) => !s.done && !s.failed && !s.cancelled);
        if (stuck) {
          stuck.failed = true;
          stuck.errorDetail = '帧缺失或引擎卡死（Blender 已退出但分段长时间无进展）';
          this.finishSegmentsError(session, stuck);
          return;
        }
      }

      const frac = this.segOutputsTotal > 0 ? Math.min(1, outputsDone / this.segOutputsTotal) : 0;
      this.emit({
        type: 'engine_progress',
        percent: frac,
        phase: 'render',
        // 每段各自的完成比例（0..1），驱动分段进度条的各个小条。
        segFracs: this.segs.map((s) =>
          s.expectedOutputs > 0 ? Math.min(1, Math.max(0, s.outputsDone / s.expectedOutputs)) : 0),
      });
      // 汇总播放头：把聚合输出比例折算成绝对帧号驱动统一的进度条/ETA。
      const frame = this.segJobRange.start +
        Math.min(this.segJobRange.totalInputs - 1,
                 Math.floor(frac * this.segJobRange.totalInputs));
      if (frame > this.segLastSyntheticFrame) {
        this.segLastSyntheticFrame = frame;
        this.emit({type: 'progress', frame, total: this.segJobRange.totalInputs});
      }

      if (settledCount === this.segs.length) {
        await this.settleSegments(session);
      }
    } finally {
      this.segPollInFlight = false;
    }
  }

  private async pullSegPreview(seg: SegRuntime): Promise<void> {
    if (!seg.rpc?.writable()) return;
    try {
      const preview = await seg.rpc.request('preview_frame', {}, 5000);
      if (preview && preview.ok && typeof preview.jpeg_b64 === 'string' && preview.jpeg_b64) {
        this.emit({
          type: 'preview_frame',
          jpeg_b64: preview.jpeg_b64,
          width: preview.width,
          height: preview.height,
        });
      }
    } catch {
      /* preview is best-effort */
    }
  }

  private async settleSegments(session: number): Promise<void> {
    if (this.segTimer) {
      clearInterval(this.segTimer);
      this.segTimer = null;
    }
    const failedSeg = this.segs.find((s) => s.failed);
    if (failedSeg) {
      this.finishSegmentsError(session, failedSeg);
      return;
    }
    const segCount = this.segs.length;
    if (this.stopping || this.segs.some((s) => s.cancelled)) {
      this.finishSegmentsStopped(session);
      return;
    }
    const videoSegs = this.segs.filter((s) => s.segVideoPath);
    if (videoSegs.length >= 2 && this.segFinalVideoPath) {
      this.emit({type: 'engine_progress', percent: 0, phase: 'finalize'});
      const first = videoSegs[0];
      try {
        if (!first.rpc?.writable()) throw new Error('CLI 已退出');
        await first.rpc.request('mov_concat', {
          inputs: videoSegs.map((s) => s.segVideoPath),
          output: this.segFinalVideoPath,
        }, 600000);
      } catch (err) {
        const detail = err instanceof Error ? err.message : String(err);
        first.failed = true;
        first.errorDetail = `分段合并失败：${detail}`;
        this.finishSegmentsError(session, first);
        return;
      }
      if (session !== this.session) return;
      debug(`mov_concat done -> ${this.segFinalVideoPath}`);
    }
    this.running = false;
    this.cleanupSegs();
    this.persistDlssconfig();
    this.emit({type: 'export_done', segments: segCount});
  }

  private finishSegmentsError(session: number, seg: SegRuntime): void {
    if (session !== this.session) return;
    if (this.segTimer) {
      clearInterval(this.segTimer);
      this.segTimer = null;
    }
    this.running = false;
    this.stopping = false;
    // fail loud: 停所有 CLI、杀所有 Blender，再回收段产物（半截 MOV/EXR 是垃圾）。
    const totalSegs = this.segs.length;
    for (const s of this.segs) {
      if (s.rpc?.writable()) {
        void s.rpc.request('stop', {}, 3000).catch(() => {});
      }
      killTree(s.blender);
    }
    this.discardSegs();
    this.cleanupSegs();
    const logHint = this.cliLog ? '（详见导出目录 cli.stderr.log）' : '';
    const detail = seg.errorDetail || 'DLSS 引擎失败';
    const message = seg.index >= 0 && totalSegs > 1
      ? `分段 ${seg.index + 1}/${totalSegs} 失败：${detail}${logHint}`
      : `${detail}${logHint}`;
    this.emit({type: 'export_error', message});
  }

  private finishSegmentsStopped(session: number, force = false): void {
    if (session !== this.session) return;
    if (force) debug('segment stop settle forced (deadline)');
    if (this.segTimer) {
      clearInterval(this.segTimer);
      this.segTimer = null;
    }
    this.running = false;
    this.stopping = false;
    this.discardSegs();
    this.cleanupSegs();
    this.persistDlssconfig();
    this.emit({type: 'stopped', ramdiskUnmounted: false, letter: this.ramLetter});
  }

  /** Kill/close every segment process; keeps the segs array for file cleanup. */
  private discardSegs(): void {
    for (const s of this.segs) {
      try { s.rpc?.close(); } catch { /* ignore */ }
      killTree(s.cli);
      killTree(s.blender);
      s.rpc = null;
      s.cli = null;
      if (s.stderrFd != null) {
        try { fs.closeSync(s.stderrFd); } catch { /* ignore */ }
        s.stderrFd = null;
      }
    }
    this.segLineBufs.clear();
  }

  /** Delete segment buffer dirs + leftover segment videos, then drop the plan. */
  private cleanupSegs(): void {
    for (const s of this.segs) {
      try { fs.rmSync(s.bufferDir, {recursive: true, force: true}); } catch { /* ramdir gone */ }
      if (s.segVideoPath) {
        try { fs.rmSync(s.segVideoPath, {force: true}); } catch { /* ignore */ }
      }
    }
    this.segs = [];
    this.segFinalVideoPath = null;
    this.segLineBufs.clear();
  }

  private cliAlive(): boolean {
    return Boolean(this.cli && this.cli.exitCode === null && this.rpc?.writable());
  }

  private abandonBlender(): void {
    killTree(this.blender);
    this.blender = null;
  }

  private discardCli(): void {
    try {
      this.rpc?.close();
    } catch {
      /* ignore */
    }
    this.rpc = null;
    killTree(this.cli);
    this.cli = null;
    this.cliPath = null;
    if (this.stderrFd != null) {
      try {
        fs.closeSync(this.stderrFd);
      } catch {
        /* ignore */
      }
      this.stderrFd = null;
    }
  }

  private async ensureCli(cli: string, exportDir: string): Promise<StdioRpcClient> {
    if (this.cliAlive() && this.cliPath === cli && this.rpc) {
      return this.rpc;
    }
    this.discardCli();
    let stderrFd: number | null = null;
    if (this.cliLog) {
      const stderrPath = path.join(exportDir, 'cli.stderr.log');
      stderrFd = fs.openSync(stderrPath, 'w');
      this.stderrFd = stderrFd;
      this.cli = spawn(cli, ['--rpc-stdio'], {
        cwd: path.dirname(cli),
        stdio: ['pipe', 'pipe', stderrFd],
        windowsHide: true,
        env: {
          ...process.env,
          // Release CLI defaults logs to off; keep stderr useful for GUI jobs.
          DLSS_COMPOSITOR_LOG: process.env.DLSS_COMPOSITOR_LOG || 'info',
        },
      });
      this.emit({type: 'cli_pid', pid: this.cli.pid, stderr: path.join(exportDir, 'cli.stderr.log')});
    } else {
      this.cli = spawn(cli, ['--rpc-stdio'], {
        cwd: path.dirname(cli),
        stdio: ['pipe', 'pipe', 'ignore'],
        windowsHide: true,
        env: process.env,
      });
      this.emit({type: 'cli_pid', pid: this.cli.pid});
    }
    this.cliPath = cli;
    const child = this.cli;
    child.on('error', (err) => debug(`cli spawn error: ${err.message}`));
    child.on('exit', (code, signal) => {
      debug(`cli exit code=${code} signal=${signal}`);
      if (this.cli === child) {
        this.rpc = null;
        this.cli = null;
        this.cliPath = null;
      }
    });
    this.rpc = new StdioRpcClient(this.cli);
    debug(`cli pid=${this.cli.pid}`);
    return this.rpc;
  }

  private async startJob(
    rpc: StdioRpcClient,
    params: Record<string, unknown>
  ): Promise<Record<string, unknown>> {
    try {
      return await rpc.request('start_job', params, 180000);
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      if (!/already running/i.test(message)) throw err;
      if (!rpc.writable()) throw err;
      await rpc.request('stop', {}, 5000);
      return rpc.request('start_job', params, 180000);
    }
  }

  private onBlenderChunk(text: string): void {
    this.blenderLineBuf += text;
    const lines = this.blenderLineBuf.split(/\r?\n/);
    this.blenderLineBuf = lines.pop() ?? '';
    for (const line of lines) {
      const stripped = line.trim();
      if (stripped.startsWith('DLSS_EVENT:')) {
        try {
          const payload = JSON.parse(stripped.slice('DLSS_EVENT:'.length)) as Record<string, unknown>;
          // Bake (写 dlssconfig) streams per-frame progress — fold it into the
          // unified export bar as the first 10% (phase "prepare").
          if (payload.event === 'bake_progress') {
            const pct = Math.min(1, Math.max(0, Number(payload.percent) || 0));
            this.emit({type: 'engine_progress', percent: pct, phase: 'prepare'});
          }
          this.emit({type: String(payload.event ?? 'blender'), ...payload});
        } catch {
          this.emit({type: 'blender_log', line: stripped.slice(0, 300)});
        }
      } else if (stripped) {
        this.emit({type: 'blender_log', line: stripped.slice(0, 300)});
      }
    }
  }

  private async pollOnce(session: number): Promise<void> {
    if (session !== this.session || !this.rpc?.writable() || this.pollInFlight || this.exportSettled) {
      return;
    }
    // Stopping must settle even if the CLI stopped answering: force at 60s.
    if (this.stopping && Date.now() > this.stopDeadline) {
      this.finishStopped(session, undefined, /*force=*/ true);
      return;
    }
    this.pollInFlight = true;
    try {
      const progress = await this.rpc.request('progress', {}, 5000);
      if (session !== this.session) return;
      // Unified export bar (single source per phase): render/finalize map the
      // CLI's encode-based percent (0-100 on the job path) into 10-90 / 90-100.
      // Prepare (bake) arrives separately as phase "prepare" from blender.
      const rawPct = typeof progress.percent === 'number' ? progress.percent : 0;
      const cliPct = Math.min(1, Math.max(0, rawPct / 100));
      const finalize = progress.phase === 1 || progress.phase === 'finalize';
      this.emit({
        type: 'engine_progress',
        percent: cliPct,
        phase: finalize ? 'finalize' : 'render',
      });
      const state = progress.state;
      const failed = state === 3 || state === 'error';
      const finished = state === 2 || state === 'done';
      // RunEvent::Cancelled leaves state Idle (0) — a settled stop.
      const cancelled = state === 0;
      const settled = failed || finished || cancelled;
      if (!settled) {
        // Preview pull only while the job is alive. It must not sit between
        // the progress read and the state check: a dead engine makes EVERY
        // preview_frame throw, which previously skipped the failed-branch on
        // every poll and left the GUI "running" forever (2026-08-31 user run).
        if (!this.stopping) {
          try {
            const preview = await this.rpc.request('preview_frame', {}, 5000);
            if (session !== this.session) return;
            if (preview && preview.ok && typeof preview.jpeg_b64 === 'string' && preview.jpeg_b64) {
              this.emit({type: 'preview_frame', jpeg_b64: preview.jpeg_b64, width: preview.width, height: preview.height});
            }
          } catch (err) {
            if (session !== this.session) return;
            this.emit({type: 'rpc_warn', message: err instanceof Error ? err.message : String(err)});
          }
        }
        return;
      }
      if (this.pollTimer) {
        clearInterval(this.pollTimer);
        this.pollTimer = null;
      }
      this.running = false;
      if (failed) this.abandonBlender();
      this.persistDlssconfig();
      void this.sweepRamDir();
      if (!this.exportSettled) {
        this.exportSettled = true;
        if (this.stopping || cancelled) {
          // User stop (or a cancel from the CLI side): the drain just
          // finished, so the stopped transition happens now, after the mov
          // trailer — not before the file actually landed.
          this.finishStopped(session, undefined);
        } else {
          const engineDetail = typeof progress.error === 'string' && progress.error
            ? String(progress.error)
            : '';
          const logHint = this.cliLog ? '（详见导出目录 cli.stderr.log）' : '';
          this.emit({
            type: failed ? 'export_error' : 'export_done',
            message: failed
              ? engineDetail
                ? `DLSS 引擎失败：${engineDetail}${logHint}`
                : `DLSS 引擎失败${logHint}`
              : undefined,
            ...progress,
          });
        }
      }
    } catch (err) {
      if (session !== this.session) return;
      if (this.stopping && Date.now() > this.stopDeadline - 55000) {
        // Swallowed RPC failures while stopping are fine — keep polling until
        // the deadline; only a dead connection needs the force path.
        if (!this.rpc?.writable()) this.finishStopped(session, undefined, true);
        return;
      }
      this.emit({type: 'rpc_warn', message: err instanceof Error ? err.message : String(err)});
    } finally {
      this.pollInFlight = false;
    }
  }

  /** Settle a stopping job: stop polling, emit the single 'stopped' event. */
  private finishStopped(session: number, warning: string | undefined, force = false): void {
    if (session !== this.session) return;
    if (force) debug('stop settle forced (deadline)');
    this.stopping = false;
    if (this.pollTimer) {
      clearInterval(this.pollTimer);
      this.pollTimer = null;
    }
    this.running = false;
    this.exportSettled = true;
    this.abandonBlender();
    this.persistDlssconfig();
    void this.sweepRamDir();
    this.emit({type: 'stopped', ramdiskUnmounted: false, letter: this.ramLetter, warning});
  }
}

export function parseCliFlags(argv: string[]): Record<string, string | boolean> {
  const out: Record<string, string | boolean> = {};
  // Works in both dev (`electron . --flag`) and packaged (`App.exe --flag`):
  // anchor on the first `--` passthrough marker or the first flag itself.
  const marker = argv.findIndex((item) => item === '--');
  const firstFlag = marker >= 0 ? marker + 1 : argv.findIndex((item) => item.startsWith('--'));
  const list = firstFlag >= 0 ? argv.slice(firstFlag) : [];
  for (let i = 0; i < list.length; i++) {
    const item = list[i];
    if (!item.startsWith('--')) continue;
    const eq = item.indexOf('=');
    if (eq > 2) {
      // `--screenshot=path` / `--dcp=path` form (installer association, shortcuts)
      out[item.slice(2, eq)] = item.slice(eq + 1);
      continue;
    }
    const key = item.slice(2);
    const next = list[i + 1];
    if (!next || next.startsWith('--')) {
      out[key] = true;
    } else {
      out[key] = next;
      i += 1;
    }
  }
  return out;
}


