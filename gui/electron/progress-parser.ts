/**
 * Parse a single stdout line from dlss-compositor.
 * Returns { current, total } if it's a progress line, null otherwise.
 */
export function parseProgressLine(line: string): { current: number; total: number } | null {
  const match = line.match(/Processing frame (\d+)\/(\d+)/)
  if (!match) return null
  return { current: parseInt(match[1], 10), total: parseInt(match[2], 10) }
}

/**
 * Buffer that reassembles partial stdout chunks into complete newline-terminated lines.
 */
export class ProgressLineBuffer {
  private buffer = ''
  public onLine: (line: string) => void = () => {}

  feed(chunk: string): void {
    this.buffer += chunk
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
      this.onLine(this.buffer.trimEnd())
      this.buffer = ''
    }
  }
}
