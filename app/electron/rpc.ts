import type {ChildProcess} from 'node:child_process';

export class RpcError extends Error {
  payload: unknown;
  constructor(message: string, payload?: unknown) {
    super(message);
    this.payload = payload;
  }
}

function encodeFrame(payload: unknown): Buffer {
  const body = Buffer.from(JSON.stringify(payload), 'utf8');
  const header = Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, 'ascii');
  return Buffer.concat([header, body]);
}

function writeStdin(stdin: NodeJS.WritableStream, frame: Buffer): Promise<void> {
  return new Promise((resolve, reject) => {
    const stream = stdin as NodeJS.WritableStream & {destroyed?: boolean; writableEnded?: boolean};
    if (!stream.writable || stream.destroyed || stream.writableEnded) {
      reject(new RpcError('RPC 连接已关闭'));
      return;
    }
    try {
      stdin.write(frame, (err) => {
        if (err) reject(err);
        else resolve();
      });
    } catch (err) {
      reject(err);
    }
  });
}

export class StdioRpcClient {
  private nextId = 1;
  private buffer = Buffer.alloc(0);
  private queue: Promise<unknown> = Promise.resolve();
  private ended = false;
  private readonly proc: ChildProcess;

  constructor(proc: ChildProcess) {
    this.proc = proc;
    if (!proc.stdout) {
      throw new RpcError('RPC stdout is not connected');
    }
    proc.stdout.on('data', (chunk: Buffer) => {
      this.buffer = Buffer.concat([this.buffer, chunk]);
    });
    proc.stdin?.on('error', () => {
      this.ended = true;
    });
    proc.on('exit', () => {
      this.ended = true;
    });
  }

  writable(): boolean {
    const stdin = this.proc.stdin;
    if (this.ended || !stdin) return false;
    if (this.proc.exitCode !== null || this.proc.killed) return false;
    return !stdin.destroyed && !stdin.writableEnded && stdin.writable;
  }

  request(method: string, params: Record<string, unknown> = {}, timeoutMs = 30000): Promise<Record<string, unknown>> {
    const run = () => this.requestOnce(method, params, timeoutMs);
    const pending = this.queue.then(run, run);
    this.queue = pending.then(
      () => undefined,
      () => undefined
    );
    return pending;
  }

  close(): void {
    this.ended = true;
    try {
      if (this.proc.stdin && !this.proc.stdin.writableEnded && !this.proc.stdin.destroyed) {
        this.proc.stdin.end();
      }
    } catch {
      /* ignore */
    }
  }

  private async requestOnce(
    method: string,
    params: Record<string, unknown>,
    timeoutMs: number
  ): Promise<Record<string, unknown>> {
    if (!this.writable() || !this.proc.stdin) {
      throw new RpcError('RPC 连接已关闭');
    }
    const id = this.nextId++;
    try {
      await writeStdin(
        this.proc.stdin,
        encodeFrame({
          jsonrpc: '2.0',
          id,
          method,
          params,
        })
      );
    } catch (err) {
      this.ended = true;
      const message = err instanceof Error ? err.message : String(err);
      throw new RpcError(message.includes('write after end') ? 'RPC 连接已关闭' : message, err);
    }
    const response = await this.readFrame(timeoutMs, id);
    if (response.error) {
      const err = response.error as {message?: string};
      throw new RpcError(String(err.message ?? err), err);
    }
    const result = (response.result ?? {}) as Record<string, unknown>;
    if (result.ok === false) {
      throw new RpcError(String(result.error ?? result), result);
    }
    return result;
  }

  private async readFrame(timeoutMs: number, expectedId: number): Promise<Record<string, unknown>> {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const parsed = this.tryParse();
      if (parsed) {
        if (parsed.id === expectedId) return parsed;
        continue;
      }
      if (this.proc.exitCode !== null) {
        throw new RpcError('RPC stdout closed');
      }
      await new Promise((r) => setTimeout(r, 20));
    }
    throw new RpcError('RPC timeout');
  }

  private tryParse(): Record<string, unknown> | null {
    const text = this.buffer.toString('utf8');
    const headerEnd = text.indexOf('\r\n\r\n');
    if (headerEnd < 0) return null;
    const header = text.slice(0, headerEnd);
    const match = header.match(/content-length:\s*(\d+)/i);
    if (!match) {
      this.buffer = this.buffer.subarray(headerEnd + 4);
      return null;
    }
    const length = Number(match[1]);
    const bodyStart = Buffer.byteLength(text.slice(0, headerEnd + 4), 'utf8');
    if (this.buffer.length < bodyStart + length) return null;
    const body = this.buffer.subarray(bodyStart, bodyStart + length).toString('utf8');
    this.buffer = this.buffer.subarray(bodyStart + length);
    return JSON.parse(body) as Record<string, unknown>;
  }
}
