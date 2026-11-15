import {spawn, type ChildProcessWithoutNullStreams} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {createInterface} from 'node:readline';
import {phaseLabel, type planRollingRender} from '../src/ramWindows';
import {captureBlenderLog} from './blenderLog';

type Plan = ReturnType<typeof planRollingRender>;
type Window = {start: number; end: number; published: number};
type Worker = {
  id: number;
  proc: ChildProcessWithoutNullStreams;
  closed: Promise<void>;
  ready: boolean;
  window: Window | null;
};

type Options = {
  blender: string;
  cwd: string;
  inputDir: string;
  logDirectory?: string;
  start: number;
  end: number;
  plan: Plan;
  args: (staging: string) => string[];
  emit: (event: {type: string; [key: string]: unknown}) => void;
  fail: (message: string) => void;
};

/** Only the producers are parallel. One watched CLI owns all NGX history,
 * output numbering and muxers. Windows reserve capacity before dispatch so a
 * fast later producer can never take the space needed by the earliest one. */
export class RollingRender {
  private workers: Worker[] = [];
  private stagingDirs: string[] = [];
  private next: number;
  private consumed = 0;
  private rendered = 0;
  private stopped = false;
  private shutdown: Promise<void> | null = null;
  private cleanup: Promise<void> | null = null;
  private lowSpaceSince = 0;

  constructor(private readonly options: Options) {
    this.next = options.start;
  }

  start(): void {
    const {plan, inputDir, start, end, blender, cwd, emit} = this.options;
    const windows = Math.ceil((end - start + 1) / plan.framesPerWindow);
    emit({type: 'ram_window_plan', windows, workers: plan.workers, frames_per_window: plan.framesPerWindow,
      buffer_slots: plan.slots, estimated_exr_bytes: plan.bytesPerFrame, ram_budget_bytes: plan.budgetBytes,
      reserve_bytes: plan.headroomBytes,
      message: `滚动窗口：${plan.workers} 个 Blender 进程 · ${windows} 窗 · 每窗最多 ${plan.framesPerWindow} 帧`});

    for (let id = 0; !this.stopped && id < Math.min(plan.workers, windows); id++) {
      const staging = path.join(inputDir, `producer-${id + 1}`);
      fs.mkdirSync(staging);
      this.stagingDirs.push(staging);
      const cmd = this.options.args(staging);
      const proc = spawn(blender, cmd, {cwd, windowsHide: true});
      if (this.options.logDirectory) {
        captureBlenderLog(proc, cmd, this.options.logDirectory, `worker-${id + 1}`);
      }
      const worker: Worker = {id, proc, ready: false, window: null,
        closed: new Promise((resolve) => proc.once('close', () => resolve()))};
      this.workers.push(worker);
      emit({type: 'blender_pid', worker: id + 1, pid: proc.pid, cmd});
      for (const stream of [proc.stdout, proc.stderr]) {
        createInterface({input: stream, crlfDelay: Infinity}).on('line', (line) => this.onLine(worker, line));
      }
      proc.on('error', (err) => this.fail(`Blender 进程 ${id + 1}：${err.message}`));
      proc.stdin.on('error', (err) => this.fail(`Blender 进程 ${id + 1} 输入：${err.message}`));
      proc.on('close', (code) => {
        if (!this.stopped) this.fail(`Blender 进程 ${id + 1} 意外退出（${code ?? '未知'}）`);
      });
    }
  }

  onConsumed(count: number): void {
    this.consumed = Math.max(this.consumed, count);
    this.dispatch();
  }

  private dispatch(): void {
    if (this.stopped) return;
    const {plan, start, end, inputDir, emit} = this.options;
    try {
      for (const worker of this.workers) {
        if (this.next > end || !worker.ready || worker.window) continue;
        const stop = Math.min(end, this.next + plan.framesPerWindow - 1);
        const count = stop - this.next + 1;
        const reserved = this.next - start - this.consumed;
        if (reserved + count > plan.slots) break;

        // Files already written use real disk space. Charge the raw estimate
        // for the remaining frames (including any write currently in progress).
        const pending = this.workers.reduce((sum, w) => sum + (w.window ?
          w.window.end - w.window.start + 1 - w.window.published : 0), 0);
        const disk = fs.statfsSync(inputDir);
        const free = disk.bavail * disk.bsize;
        if (free < plan.headroomBytes + (pending + count) * plan.bytesPerFrame) {
          if (!this.lowSpaceSince) {
            this.lowSpaceSince = Date.now();
            emit({type: 'buffer_full', free, floor: plan.headroomBytes + (pending + count) * plan.bytesPerFrame});
          }
          // If the oldest dispatched work is drained, there is nothing left
          // to free. Fail immediately instead of leaving the CLI waiting.
          if (reserved <= 0 || Date.now() - this.lowSpaceSince > 30000) {
            this.fail('缓存可用空间不足，无法继续分配渲染窗口');
          }
          break;
        }
        if (this.lowSpaceSince) {
          this.lowSpaceSince = 0;
          emit({type: 'buffer_recovered', free});
        }
        const window: Window = {start: this.next, end: stop, published: 0};
        worker.window = window;
        this.next = stop + 1;
        emit({type: 'job_phase', phase: 'combined', message: phaseLabel('combined', window.start, window.end),
          frame_start: window.start, frame_end: window.end, worker: worker.id + 1,
          reserved_frames: reserved + count});
        worker.proc.stdin.write(JSON.stringify({start: window.start, end: window.end}) + '\n');
      }
    } catch (err) {
      this.fail(err instanceof Error ? err.message : String(err));
    }
  }

  private onLine(worker: Worker, line: string): void {
    if (this.stopped || !line.trim()) return;
    if (!line.startsWith('DLSS_EVENT:')) {
      this.options.emit({type: 'blender_log', worker: worker.id + 1, line: line.slice(0, 300)});
      return;
    }
    try {
      const payload = JSON.parse(line.slice('DLSS_EVENT:'.length));
      const window = worker.window;
      switch (payload.event) {
        case 'worker_ready':
          worker.ready = true;
          this.dispatch();
          break;
        case 'worker_frame':
          if (!window || payload.frame !== window.start + window.published || payload.frame > window.end) {
            throw new Error(`收到窗口之外的帧 ${payload.frame}`);
          }
          window.published++;
          // Physical frames may finish out of order; use a monotonic count
          // for the UI while the CLI separately reports encoded progress.
          this.options.emit({type: 'frame_done', frame: this.options.start + this.rendered++,
            total: this.options.end - this.options.start + 1, source_frame: payload.frame, worker: worker.id + 1});
          break;
        case 'worker_done':
          if (!window || payload.start !== window.start || payload.end !== window.end ||
              window.published !== window.end - window.start + 1) {
            throw new Error('渲染窗口结束但 EXR 帧数不符');
          }
          worker.window = null;
          this.dispatch();
          break;
        case 'error':
          this.fail(`Blender 进程 ${worker.id + 1}：${payload.message ?? '渲染失败'}`);
          break;
        case 'buffer_full':
        case 'buffer_recovered':
          this.options.emit({type: payload.event, ...payload, worker: worker.id + 1});
          break;
      }
    } catch (err) {
      this.fail(`Blender 进程 ${worker.id + 1}：${err instanceof Error ? err.message : String(err)}`);
    }
  }

  private fail(message: string): void {
    if (!this.stopped) this.options.fail(message);
  }

  stopProducers(abort: boolean): Promise<void> {
    if (this.shutdown) return this.shutdown;
    this.stopped = true;
    this.shutdown = (async () => {
      for (const worker of this.workers) {
        worker.proc.stdin.end();
        if (abort && worker.proc.exitCode === null) worker.proc.kill();
      }
      const deadline = setTimeout(() => {
        for (const worker of this.workers) if (worker.proc.exitCode === null) worker.proc.kill();
      }, 5000);
      try {
        await Promise.all(this.workers.map((worker) => worker.closed));
      } finally {
        clearTimeout(deadline);
      }
    })();
    return this.shutdown;
  }

  /** Call after the consumer settles. Wait for writers before deleting EXRs. */
  close(abort: boolean): Promise<void> {
    if (this.cleanup) return this.cleanup;
    this.cleanup = (async () => {
      await this.stopProducers(abort);
      // Only this job's generated files may be removed. Do not recursively
      // remove a user-selected cache path.
      for (const dir of [...this.stagingDirs, this.options.inputDir]) {
        try {
          for (const file of fs.readdirSync(dir)) {
            if (/^-?\d+\.exr$/i.test(file)) fs.unlinkSync(path.join(dir, file));
          }
          fs.rmdirSync(dir);
        } catch { /* keep unrelated files and tolerate a disappearing RAM disk */ }
      }
    })();
    return this.cleanup;
  }
}
