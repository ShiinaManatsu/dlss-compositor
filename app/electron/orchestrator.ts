import {spawn, type ChildProcess} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {app} from 'electron';
import {parseFg} from '../src/dcp';
import {userErrorMessage} from '../src/errors';
import {estimateRollingFrameBytes, estimateWindowFrameBytes, phaseLabel, planRamWindows, planRollingRender, ramWindowBudget} from '../src/ramWindows';
import {RollingRender} from './rollingRender';
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
  /** Export the MV/depth guide video for the NR tool. */
  nrBuffer?: boolean;
  /** Session perf fallbacks for inject.py (defaults match its fallback set). */
  perfPersistent?: boolean;
  perfCameraCull?: boolean;
  cameraCullMargin?: number;
  perfDistanceCull?: boolean;
  distanceCullMargin?: number;
  perfTileSize?: number;
  /** 滚动窗口：Blender 并行进程数（2..6）；旧项目沿用此字段。 */
  parallelSegments?: number;
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
  'C:\\Program Files\\Blender Foundation\\Blender 5.3\\blender.exe',
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
  private rolling: RollingRender | null = null;
  private producerError: string | undefined;

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
    if (!Number.isSafeInteger(req.frameStart) || !Number.isSafeInteger(req.frameEnd) || req.frameEnd < req.frameStart) {
      return {ok: false, error: '渲染帧范围无效'};
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
    this.producerError = undefined;
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
      if (session !== this.session || !this.running) return {ok: false, error: '导出已停止'};
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
      const workerCount = Math.min(6, Math.trunc(req.parallelSegments ?? 0));
      if (workerCount >= 2 && dual) {
        throw new Error('滚动并行暂不支持 SR 的 Beauty + Guides 双趟，请关闭原生分辨率缓冲');
      }
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
      if (session !== this.session || !this.running) return {ok: false, error: '导出已停止'};
      const configPath = path.join(ramDir, 'dlssconfig.json');
      if (!existsFile(configPath)) {
        throw new Error('inject 未写出 dlssconfig.json');
      }

      let inputDir = ramDir;
      if (workerCount >= 2) {
        const config = JSON.parse(fs.readFileSync(configPath, 'utf8'));
        const {width, height} = config.render_resolution ?? {};
        if (!Number.isSafeInteger(width) || width <= 0 || !Number.isSafeInteger(height) || height <= 0) {
          throw new Error('无法读取 EXR 的实际渲染分辨率');
        }
        const disk = fs.statfsSync(ramDir);
        const physicalBytes = disk.blocks * disk.bsize;
        const cacheCap = Math.max(2, Math.min(64, req.ramdiskGb || 8)) * 1024 ** 3;
        const volumeBytes = req.useRamdisk ? physicalBytes : Math.min(physicalBytes, cacheCap);
        const maxFrames = req.ramWindowFrames && Math.max(1, Math.trunc(req.ramWindowFrames));
        const plan = planRollingRender(req.frameEnd - req.frameStart + 1, workerCount,
          estimateRollingFrameBytes(width, height, req.mode), volumeBytes, disk.bavail * disk.bsize, maxFrames || undefined);
        const rollingDir = fs.mkdtempSync(path.join(ramDir, '.roll-'));
        inputDir = rollingDir;
        this.rolling = new RollingRender({blender, cwd: backendRoot(), inputDir: rollingDir,
          start: req.frameStart, end: req.frameEnd, plan,
          args: (staging) => [
            ...this.buildInjectArgs(req, staging, 'combined', pct, {
              frameStart: req.frameStart, frameEnd: req.frameEnd, skipConfig: true, configDir: ramDir,
            }),
            '--worker-publish-dir', rollingDir,
            '--worker-free-floor', String(plan.headroomBytes + plan.bytesPerFrame),
          ],
          emit: (event) => { if (session === this.session) this.emit(event); },
          fail: (message) => this.failProducer(session, message),
        });
      }

      const jobParams: Record<string, unknown> = {
        mode: req.mode,
        input_dir: inputDir,
        output_dir: exportDir,
        output_format: req.outputFormat,
        output_template: outputTemplate,
        frame_start: req.frameStart,
        frame_end: req.frameEnd,
        window: req.window ?? 6,
        watch: true,
        wait_for_appearance: true,
        delete_consumed: true,
        atomic_input_files: Boolean(this.rolling),
        native_res_buffers: native,
        nr_buffer: Boolean(req.nrBuffer),
        nr_buffer_dir: nrBufferDir,
        color_dir: dual ? colorDir : undefined,
        guides_dir: dual ? guidesDir : undefined,
        config: configPath,
        rr_preset: 'F',
        overwrite_mode: 'overwrite',
      };
      if (fg >= 2) jobParams.fg_multiplier = fg;
      const job = await this.startJob(rpc, jobParams);
      if (session !== this.session || !this.running) return {ok: false, error: '导出已停止'};
      debug(`start_job ${JSON.stringify(job)}`);
      this.emit({type: 'job_started', ...job, native_res_buffers: native, dual_pass: dual, fg});

      this.pollTimer = setInterval(() => {
        void this.pollOnce(session);
      }, 400);

      if (this.rolling) {
        this.rolling.start();
      } else if (native) {
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
      if (session !== this.session || this.stopping) return {ok: false, error: message};
      if (this.pollTimer) {
        this.failProducer(session, message);
      } else {
        this.abandonBlender();
        await this.closeRolling(true);
        this.running = false;
        this.emit({type: 'error', message});
      }
      return {ok: false, error: message};
    }
  }

  async stop(): Promise<{ok: boolean; unmounted: false}> {
    const hadWork = this.running || this.pollTimer != null;
    if (this.stopping || (this.exportSettled && hadWork)) return {ok: true, unmounted: false};
    const polling = this.pollTimer != null;
    if (hadWork) {
      this.stopping = true;
      this.stopDeadline = Date.now() + 60000;
      if (!polling) this.session += 1;  // invalidate an in-flight bake or start_job
    }
    this.abandonBlender();
    void this.rolling?.stopProducers(true);  // clean up after engine settles
    let warning: string | undefined;
    if (hadWork && this.rpc?.writable()) {
      try {
        await this.rpc.request('stop', {}, 5000);
      } catch (err) {
        warning = err instanceof Error ? err.message : String(err);
        if (warning.includes('write after end')) warning = 'RPC 连接已关闭';
        debug(`stop rpc: ${warning}`);
      }
    } else if (hadWork && this.cli && !this.rpc?.writable()) {
      warning = 'RPC 连接已关闭';
    }
    if (hadWork && !polling) {
      await this.settleExport(this.session, 'stopped', {}, warning);
    }
    return {ok: true, unmounted: false};
  }

  private failProducer(session: number, message: string): void {
    if (session !== this.session || this.stopping || this.exportSettled) return;
    this.producerError = message;
    void this.stop();
  }

  private async closeRolling(abort: boolean): Promise<void> {
    const rolling = this.rolling;
    if (!rolling) return;
    await rolling.close(abort);
    if (this.rolling === rolling) this.rolling = null;
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
        await this.stop();
        // Kill both CLIs before unmounting: their RAM-disk watcher/file
        // handles would make the graceful imdisk detach fail.
        this.discardNrCli();
        this.discardCli();
        await this.closeRolling(true);
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
      '--python-exit-code', '1',
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
    this.blender = proc;
    proc.on('error', (err) => debug(`bake Blender spawn: ${err.message}`));
    proc.stdout?.on('data', (chunk: Buffer) => this.onBlenderChunk(chunk.toString('utf8')));
    proc.stderr?.on('data', (chunk: Buffer) => this.onBlenderChunk(chunk.toString('utf8')));
    this.emit({type: 'blender_start', message: '写 dlssconfig / LUT，不渲染', cmd: args});
    const code = await this.waitBlender(proc);
    if (this.blender === proc) this.blender = null;
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
      if (session !== this.session || !this.running || this.stopping) return;
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
          this.failProducer(session, `Beauty 趟失败（${beautyCode}）`);
          return;
        }
        wroteConfig = true;
        if (session !== this.session || !this.running || this.stopping) return;
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
          this.failProducer(session, `Guides 趟失败（${guidesCode}）`);
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
          this.failProducer(session, `Blender 失败（${code}）`);
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
    this.cli?.kill();
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
    if (session !== this.session || this.pollInFlight || this.exportSettled) {
      return;
    }
    const rpc = this.rpc;
    if (!rpc?.writable()) {
      await this.settleExport(session, this.stopping ? 'stopped' : 'error', {error: 'CLI 已退出'});
      return;
    }
    if (this.stopping && Date.now() > this.stopDeadline) {
      this.discardCli();
      await this.settleExport(session, 'stopped', {}, 'CLI 收尾超时');
      return;
    }
    this.pollInFlight = true;
    try {
      const progress = await rpc.request('progress', {}, 5000);
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
      const cancelled = state === 0 && this.stopping && progress.job_running !== true;
      const settled = failed || finished || cancelled;
      if (!settled) {
        // Preview pull only while the job is alive. It must not sit between
        // the progress read and the state check: a dead engine makes EVERY
        // preview_frame throw, which previously skipped the failed-branch on
        // every poll and left the GUI "running" forever (2026-08-31 user run).
        if (!this.stopping) {
          if (typeof progress.dlss_current_frame === 'number') this.rolling?.onConsumed(progress.dlss_current_frame);
          try {
            const preview = await rpc.request('preview_frame', {}, 5000);
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
      await this.settleExport(session, this.stopping || cancelled ? 'stopped' : failed ? 'error' : 'done', progress);
    } catch (err) {
      if (session !== this.session) return;
      if (this.stopping && Date.now() > this.stopDeadline - 55000) {
        // Swallowed RPC failures while stopping are fine — keep polling until
        // the deadline; only a dead connection needs the force path.
        if (!rpc.writable()) await this.settleExport(session, 'stopped');
        return;
      }
      this.emit({type: 'rpc_warn', message: err instanceof Error ? err.message : String(err)});
    } finally {
      this.pollInFlight = false;
    }
  }

  private async settleExport(session: number, outcome: 'done' | 'stopped' | 'error',
    progress: Record<string, unknown> = {}, warning?: string): Promise<void> {
    if (session !== this.session || this.exportSettled) return;
    this.exportSettled = true;
    if (this.pollTimer) {
      clearInterval(this.pollTimer);
      this.pollTimer = null;
    }
    if (outcome !== 'done') this.abandonBlender();
    await this.closeRolling(outcome !== 'done');
    this.persistDlssconfig();
    await this.sweepRamDir();
    this.running = false;
    this.stopping = false;
    if (this.producerError || outcome === 'error') {
      const detail = this.producerError ?? (typeof progress.error === 'string' ? progress.error : 'DLSS 引擎失败');
      this.emit({...progress, type: 'export_error',
        message: `${detail}${this.cliLog ? '（详见导出目录 cli.stderr.log）' : ''}`});
    } else if (outcome === 'stopped') {
      this.emit({type: 'stopped', ramdiskUnmounted: false, letter: this.ramLetter, warning});
    } else {
      this.emit({...progress, type: 'export_done'});
    }
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


