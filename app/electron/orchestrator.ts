import {spawn, type ChildProcess} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {fgRpcValue, parseFg} from '../src/dcp';
import {userErrorMessage} from '../src/errors';
import {estimateWindowFrameBytes, phaseLabel, planRamWindows, ramWindowBudget} from '../src/ramWindows';
import {StdioRpcClient} from './rpc';

function debug(message: string): void {
  try {
    const log = path.resolve(repoRoot(), 'build', 'stage4-out', 'app-debug.log');
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

const IMDISK_MISSING = '请安装 ImDisk Toolkit';
const NR_RESERVED = 'DLSS5-NR 模型未到，接口已预留';
const VOLUME_LABEL = 'DLSSRAM';

const BLENDER_CANDIDATES = [
  path.join('D:', 'Games', 'SteamLibrary', 'steamapps', 'common', 'Blender', 'blender.exe'),
  'C:\\Program Files\\Blender Foundation\\Blender 5.2\\blender.exe',
  'C:\\Program Files\\Blender Foundation\\Blender 5.1\\blender.exe',
];

const CLI_CANDIDATES = [
  'build/x64-windows-msvc-release/Release/dlss-compositor-cli.exe',
  'build/review/Release/dlss-compositor-cli.exe',
  'build/Release/dlss-compositor-cli.exe',
];

function repoRoot(): string {
  const here = path.dirname(fileURLToPath(import.meta.url));
  return path.resolve(here, '..', '..');
}

function pythonBin(): string {
  return process.env.PYTHON || 'python';
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

function findCli(explicit?: string): string | null {
  if (explicit && existsFile(explicit)) return explicit;
  const env = process.env.DLSS_COMPOSITOR_CLI;
  if (env && existsFile(env)) return env;
  const root = repoRoot();
  for (const candidate of CLI_CANDIDATES) {
    const full = path.isAbsolute(candidate) ? candidate : path.join(root, candidate);
    if (existsFile(full)) return full;
  }
  return null;
}

function parsePythonJsonLines(stderr: string): Record<string, unknown>[] {
  const rows: Record<string, unknown>[] = [];
  for (const line of stderr.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (!trimmed.startsWith('{')) continue;
    try {
      rows.push(JSON.parse(trimmed) as Record<string, unknown>);
    } catch {
      /* ignore */
    }
  }
  return rows;
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
  timeoutMs: number
): Promise<{code: number; stdout: string; stderr: string}> {
  return new Promise((resolve, reject) => {
    const proc = spawn(exe, args, {
      cwd: repoRoot(),
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

async function runPython(args: string[], timeoutMs = 15000): Promise<{code: number; stdout: string; stderr: string}> {
  return runCommand(pythonBin(), args, timeoutMs);
}

export class PipelineOrchestrator {
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

  onEvent(listener: (event: PipelineEvent) => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private emit(event: PipelineEvent): void {
    for (const listener of this.listeners) listener(event);
  }

  async probe(): Promise<ProbeResult> {
    const blender = findBlender();
    const cli = findCli();
    let imdisk: ProbeResult['imdisk'] = {ok: false, message: IMDISK_MISSING};
    let ramdisk: ProbeResult['ramdisk'] = {ok: false, message: `未找到卷标 ${VOLUME_LABEL} 的 RAM 盘`};

    try {
      const status = await runPython(['pipeline/ramdisk.py', 'status']);
      if (status.code === 2 || status.stderr.includes(IMDISK_MISSING) || status.stdout.includes(IMDISK_MISSING)) {
        imdisk = {ok: false, message: IMDISK_MISSING};
      } else if (status.code === 0) {
        imdisk = {ok: true, path: status.stdout.trim() || undefined};
      } else {
        const events = parsePythonJsonLines(status.stderr);
        const missing = events.find((row) => row.event === 'imdisk_missing' || row.event === 'error');
        imdisk = {
          ok: false,
          message: userErrorMessage(String(missing?.message ?? (status.stderr.trim() || IMDISK_MISSING))),
        };
      }
    } catch (err) {
      imdisk = {ok: false, message: userErrorMessage(err instanceof Error ? err.message : IMDISK_MISSING)};
    }

    try {
      const discovered = await runPython(['pipeline/ramdisk.py', 'discover', '--letter', 'R']);
      const events = parsePythonJsonLines(discovered.stderr);
      const ready = events.find((row) => row.event === 'ramdisk_discovered');
      if (discovered.code === 0 && ready) {
        ramdisk = {
          ok: true,
          letter: String(ready.letter ?? 'R'),
          root: String(ready.root ?? 'R:\\'),
          label: String(ready.label ?? VOLUME_LABEL),
          filesystem: String(ready.filesystem ?? ''),
          totalBytes: Number(ready.total_bytes ?? 0) || undefined,
          freeBytes: Number(ready.free_bytes ?? 0) || undefined,
        };
        this.ramLetter = ramdisk.letter ?? 'R';
      } else {
        const errEvent = events.find((row) => row.event === 'error');
        ramdisk = {
          ok: false,
          message: userErrorMessage(
            String(errEvent?.message ?? (discovered.stderr.trim() || discovered.stdout.trim() || `未找到 ${VOLUME_LABEL}`)),
            'R'
          ),
        };
      }
    } catch (err) {
      ramdisk = {
        ok: false,
        message: userErrorMessage(err instanceof Error ? err.message : `未找到 ${VOLUME_LABEL}`, 'R'),
      };
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
    const outDir = path.join(repoRoot(), 'build', 'stage4-out');
    fs.mkdirSync(outDir, {recursive: true});
    const jsonOut = path.join(outDir, `inspect-${process.pid}-${Date.now()}.json`);
    const inspectPy = path.join(repoRoot(), 'pipeline', 'inspect_blend.py');
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
    const probe = await this.probe();
    if (!probe.ramdisk.ok) {
      const message = userErrorMessage(
        probe.imdisk.ok
          ? probe.ramdisk.message || `未找到 ${VOLUME_LABEL}`
          : probe.imdisk.message || IMDISK_MISSING,
        String(probe.ramdisk.letter || 'R')
      );
      return {ok: false, error: message};
    }
    const blender = findBlender(req.blender);
    const cli = findCli(req.cli);
    if (!blender) return {ok: false, error: '找不到 blender.exe'};
    if (!cli) return {ok: false, error: '找不到 dlss-compositor-cli.exe'};

    const letter = (probe.ramdisk.letter || 'R').replace(':', '');
    const ramDir = path.join(`${letter}:\\`, 'dlss');
    const exportDir = path.resolve(req.exportDir);
    if (path.parse(exportDir).root.toUpperCase().startsWith(`${letter}:`)) {
      return {ok: false, error: '导出目录必须在工作盘，不能写到 RAM 盘'};
    }
    fs.mkdirSync(ramDir, {recursive: true});
    fs.mkdirSync(exportDir, {recursive: true});
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
      const rpc = await this.ensureCli(cli, exportDir);
      const native = Boolean(req.nativeResBuffers);
      let pct = req.effectivePercentage ?? mergeOverrides(req).percentage ?? req.percentage ?? 100;
      if (native && req.mode === 'rr') pct = 100;
      const dual = native && req.mode === 'sr' && pct < 100;
      const colorDir = path.join(ramDir, 'color');
      const guidesDir = path.join(ramDir, 'guides');
      if (dual) {
        fs.mkdirSync(colorDir, {recursive: true});
        fs.mkdirSync(guidesDir, {recursive: true});
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
        delete_consumed: true,
        wait_for_appearance: true,
        native_res_buffers: native,
        color_dir: dual ? colorDir : undefined,
        guides_dir: dual ? guidesDir : undefined,
        config: path.join(ramDir, 'dlssconfig.json'),
      };
      const fgFlag = fgRpcValue(fg);
      if (fgFlag) jobParams.fg = fgFlag;
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
          Number(probe.ramdisk.totalBytes ?? 0)
        );
      } else {
        this.spawnBlender(blender, this.buildInjectArgs(req, ramDir, 'combined', pct));
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
    }
    return {ok: true, unmounted: false};
  }

  async shutdown(): Promise<{ok: boolean; unmounted: false}> {
    const result = await this.stop();
    this.discardCli();
    return result;
  }

  private blenderLineBuf = '';

  private buildInjectArgs(
    req: ExportRequest,
    outputDir: string,
    bufferPass: 'combined' | 'beauty' | 'guides',
    percentage: number | undefined,
    extra?: {frameStart: number; frameEnd: number; skipConfig?: boolean; configDir?: string}
  ): string[] {
    const inject = path.join(repoRoot(), 'pipeline', 'inject.py');
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
    if (req.mode === 'rr') args.push('--engine', 'CYCLES');
    if (req.outputWidth) args.push('--output-width', String(req.outputWidth));
    if (req.outputHeight) args.push('--output-height', String(req.outputHeight));
    if (bufferPass !== 'combined') args.push('--pass', bufferPass);
    if (extra?.skipConfig) args.push('--skip-config');
    if (extra?.configDir) args.push('--config-dir', extra.configDir);
    return args;
  }

  private spawnBlender(blender: string, blenderArgs: string[]): ChildProcess {
    debug(`inject ${JSON.stringify(blenderArgs)}`);
    this.abandonBlender();
    this.blender = spawn(blender, blenderArgs, {
      cwd: repoRoot(),
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
    let wroteConfig = false;
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
      if (preview && (preview.path || preview.generation)) {
        this.emit({type: 'preview_frame', ...preview});
      }
      const state = progress.state;
      const blenderDone = this.blender == null || this.blender.exitCode !== null;
      if (blenderDone && (state === 2 || state === 3 || state === 'done' || state === 'error')) {
        if (this.pollTimer) {
          clearInterval(this.pollTimer);
          this.pollTimer = null;
        }
        this.running = false;
        if (!this.exportSettled) {
          this.exportSettled = true;
          this.emit({
            type: state === 3 || state === 'error' ? 'export_error' : 'export_done',
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
  const start = argv.findIndex((item) => item === '--');
  const list = start >= 0 ? argv.slice(start + 1) : argv.slice(2);
  for (let i = 0; i < list.length; i++) {
    const item = list[i];
    if (!item.startsWith('--')) continue;
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


