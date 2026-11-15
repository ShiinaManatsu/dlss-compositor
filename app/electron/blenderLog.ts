import type {ChildProcess} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {StringDecoder} from 'node:string_decoder';

export function captureBlenderLog(
  proc: ChildProcess,
  args: string[],
  directory: string,
  label: string
): void {
  const started = Date.now();
  const processLabel = `${label}-${proc.pid ?? started}`;
  const logFile = path.join(directory, `blender-${processLabel}.log`);
  const crashFiles = new Set<string>();
  const blendIndex = args.indexOf('-b');
  if (blendIndex >= 0 && args[blendIndex + 1]) {
    const name = path.basename(args[blendIndex + 1], '.blend');
    crashFiles.add(path.join(os.tmpdir(), `${name}.crash.txt`));
  }

  function append(text: string): void {
    try {
      fs.mkdirSync(directory, {recursive: true});
      fs.appendFileSync(logFile, text);
    } catch { /* Continue rendering if the log directory becomes unavailable. */ }
  }

  append(`${new Date(started).toISOString()} Blender ${processLabel}\n${JSON.stringify({args})}\n`);
  for (const [name, stream] of [['stdout', proc.stdout], ['stderr', proc.stderr]] as const) {
    if (!stream) continue;
    const decoder = new StringDecoder('utf8');
    let lineBuffer = '';
    function record(text: string): void {
      if (!text) return;
      lineBuffer += text;
      const lines = lineBuffer.split(/\r?\n/);
      lineBuffer = lines.pop() ?? '';
      for (const line of lines) {
        append(`[${name}] ${line}\n`);
        const match = line.match(/Writing:\s*["']?(.+?\.crash\.txt)["']?\s*$/i);
        if (match && path.isAbsolute(match[1])) crashFiles.add(match[1]);
      }
    }
    stream.on('data', (chunk: Buffer) => record(decoder.write(chunk)));
    stream.on('end', () => {
      record(decoder.end());
      if (lineBuffer) record('\n');
    });
  }
  proc.on('error', (error) => append(`\n${new Date().toISOString()} Spawn error ${error.stack ?? error.message}\n`));
  proc.on('close', (code, signal) => {
    append(`\n${new Date().toISOString()} Exit code=${code} signal=${signal}\n`);
    for (const source of crashFiles) {
      try {
        const stat = fs.statSync(source);
        if (!stat.isFile() || stat.mtimeMs < started - 2000) continue;
        const target = path.join(directory, `blender-${processLabel}-${path.basename(source)}`);
        fs.copyFileSync(source, target);
        append(`Crash report ${target}\n`);
      } catch (error) {
        if ((error as NodeJS.ErrnoException).code !== 'ENOENT') {
          append(`Crash report collection failed ${source} ${String(error)}\n`);
        }
      }
    }
  });
}
