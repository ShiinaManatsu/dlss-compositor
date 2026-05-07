/**
 * Parse a single stdout line from dlss-compositor.
 * Returns { current, total } if it's a progress line, null otherwise.
 *
 * @deprecated Use parseNdjsonLine — the GUI now passes --progress-format json
 *             so the CLI emits structured NDJSON events instead of human prose.
 *             Kept for backward compatibility with old binaries.
 */
export function parseProgressLine(line: string): { current: number; total: number } | null {
  const normalizedLine = line.replace(/\0/g, '')
  const match = normalizedLine.match(/Processing frame(?: pair)? (\d+)\/(\d+)/)
  if (!match) return null
  return { current: parseInt(match[1], 10), total: parseInt(match[2], 10) }
}

/**
 * NDJSON event emitted by dlss-compositor with --progress-format json.
 */
export type ProgressEvent =
  | { event: 'started'; total: number; input_dir: string; output_dir: string }
  | { event: 'frame_started'; n: number; total: number; timestamp_ms: number }
  | { event: 'frame_written'; n: number; path: string; timestamp_ms: number }
  | { event: 'banner'; sdk: string; engine: string; sr_available: boolean; fg_available: boolean; max_multi_frame: number }
  | { event: 'error'; frame?: number; message: string }
  | { event: 'complete'; total: number; duration_ms: number; errors: number };

/**
 * Parse a single NDJSON line from dlss-compositor stdout.
 * Returns the typed event, or null if the line is not valid JSON or
 * does not have a string `event` field.
 */
export function parseNdjsonLine(line: string): ProgressEvent | null {
  try {
    const obj = JSON.parse(line);
    if (obj && typeof obj.event === 'string') {
      return obj as ProgressEvent;
    }
    return null;
  } catch {
    return null;
  }
}

/**
 * Buffer that reassembles partial stdout chunks into complete newline-terminated lines.
 */
export class ProgressLineBuffer {
  private buffer = ''
  public onLine: (line: string) => void = () => {}

  feed(chunk: string): void {
    this.buffer += chunk.replace(/\0/g, '')
    const lines = this.buffer.split('\n')
    // All but the last element are complete lines
    for (let i = 0; i < lines.length - 1; i++) {
      const line = lines[i].trimEnd() // remove trailing \r on Windows
      if (line.length > 0) {
        this.onLine(line)
      }
    }
    // Keep the incomplete last fragment
    this.buffer = lines[lines.length - 1]
  }

  flush(): void {
    if (this.buffer.length > 0) {
      const line = this.buffer.replace(/\0/g, '').trimEnd()
      if (line.length > 0) {
        this.onLine(line)
      }
      this.buffer = ''
    }
  }
}
