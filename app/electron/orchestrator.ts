import {spawn, type ChildProcess} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
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
  fg?: number | string;
  effectivePercentage?: number;
  ramWindowFrames?: number;
  /** Input buffer: RAM disk (auto-created when packaged) or a plain cache dir. */
  useRamdisk: boolean;
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

const QUALITY_PERCENT: Record<string, number> = {
  DLAA: 100,
  ULTRA_QUALITY: 77,
  QUALITY: 67,
  BALANCED: 58,
  PERFORMANCE: 50,
  ULTRA_PERFORMANCE: 33,
};

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
  private pollTimer: NodeJS.Timeout | null = null;
  private running = false;
  private ramLetter: string | null = null;
  private cliPath: string | null = null;
  private stderrFd: number | null = null;
  private session = 0;
  private pollInFlight = false;
  private exportSettled = false;
  private ramDir: string | null = null;
  private ramdiskManaged = false;
  private lastRamdiskBytes = 0;

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
      const prepared = await this.prepareRamdisk();
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
        pct = QUALITY_PERCENT[req.quality.toUpperCase()] ?? 58;
      }
      const dual = native && req.mode === 'sr' && pct < 100;
      const colorDir = path.join(ramDir, 'color');
      const guidesDir = path.join(ramDir, 'guides');
      if (dual) {
        fs.mkdirSync(colorDir, {recursive: true});
        fs.mkdirSync(guidesDir, {recursive: true});
      }
      await this.bakeConfig(blender, req, ramDir, pct);
      if (!existsFile(path.join(ramDir, 'dlssconfig.json'))) {
        throw new Error('inject 未写出 dlssconfig.json');
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
      this.emit({type: 'error', message});
      return {ok: false, error: message};
    }
  }

  async stop(): Promise<{ok: boolean; unmounted: false}> {
    const hadWork = this.running || this.blender != null || this.pollTimer != null;
    this.session += 1;
    if (this.pollTimer) {
      clearInterval(this.pollTimer);
      this.pollTimer = null;
    }
    this.abandonBlender();
    let warning: string | undefined;
    if (this.rpc?.writable()) {
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
    if (hadWork) {
      this.emit({type: 'stopped', ramdiskUnmounted: false, letter: this.ramLetter, warning});
      void this.sweepRamDir();
    }
    return {ok: true, unmounted: false};
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

  async shutdown(): Promise<{ok: boolean; unmounted: false}> {
    const result = await this.stop();
    await this.releaseRamdisk();
    this.discardCli();
    return result;
  }

  /** Find an existing DLSSRAM disk; in packaged builds create one when missing
   *  (imdisk needs an elevated token). Dev builds never create. */
  private async prepareRamdisk(): Promise<{ok: boolean; letter?: string; error?: string}> {
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
    const created = await createRamdisk('8G');
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
    const stderrPath = path.join(exportDir, 'cli.stderr.log');
    this.stderrFd = fs.openSync(stderrPath, 'w');
    this.cli = spawn(cli, ['--rpc-stdio'], {
      cwd: path.dirname(cli),
      stdio: ['pipe', 'pipe', this.stderrFd],
      windowsHide: true,
      env: {
        ...process.env,
        // Release CLI defaults logs to off; keep stderr useful for GUI jobs.
        DLSS_COMPOSITOR_LOG: process.env.DLSS_COMPOSITOR_LOG || 'info',
      },
    });
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
    this.emit({type: 'cli_pid', pid: this.cli.pid, stderr: stderrPath});
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
    this.pollInFlight = true;
    try {
      const progress = await this.rpc.request('progress', {}, 5000);
      if (session !== this.session) return;
      this.emit({type: 'engine_progress', ...progress});
      const preview = await this.rpc.request('preview_frame', {}, 5000);
      if (session !== this.session) return;
      if (preview && preview.ok && typeof preview.jpeg_b64 === 'string' && preview.jpeg_b64) {
        this.emit({type: 'preview_frame', jpeg_b64: preview.jpeg_b64, width: preview.width, height: preview.height});
      }
      const state = progress.state;
      const failed = state === 3 || state === 'error';
      const finished = state === 2 || state === 'done';
      const blenderDone = this.blender == null || this.blender.exitCode !== null;
      if (failed || (blenderDone && finished)) {
        if (this.pollTimer) {
          clearInterval(this.pollTimer);
          this.pollTimer = null;
        }
        this.running = false;
        if (failed) this.abandonBlender();
        void this.sweepRamDir();
        if (!this.exportSettled) {
          this.exportSettled = true;
          this.emit({
            type: failed ? 'export_error' : 'export_done',
            message: failed ? 'DLSS 引擎失败（见导出目录 cli.stderr.log）' : undefined,
            ...progress,
          });
        }
      }
    } catch (err) {
      if (session !== this.session) return;
      this.emit({type: 'rpc_warn', message: err instanceof Error ? err.message : String(err)});
    } finally {
      this.pollInFlight = false;
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


