export const RR_FG_CREATE_FAILED = 'RR+FG 创建失败';

const DRIVE_UNMOUNTED = /drive\s+([A-Za-z]):\s+is not mounted/i;

function firstJsonObject(text: string): Record<string, unknown> | null {
  if (!text.startsWith('{')) return null;
  try {
    const obj = JSON.parse(text) as unknown;
    return obj && typeof obj === 'object' && !Array.isArray(obj) ? (obj as Record<string, unknown>) : null;
  } catch {
    /* trailing junk after a JSON object — DC-25 footer blob */
  }
  let depth = 0;
  let inStr = false;
  let escape = false;
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (inStr) {
      if (escape) escape = false;
      else if (ch === '\\') escape = true;
      else if (ch === '"') inStr = false;
      continue;
    }
    if (ch === '"') inStr = true;
    else if (ch === '{') depth += 1;
    else if (ch === '}') {
      depth -= 1;
      if (depth === 0) {
        try {
          const obj = JSON.parse(text.slice(0, i + 1)) as unknown;
          return obj && typeof obj === 'object' && !Array.isArray(obj)
            ? (obj as Record<string, unknown>)
            : null;
        } catch {
          return null;
        }
      }
    }
  }
  return null;
}

function jsonErrorText(raw: string): string | null {
  const candidates = [raw, ...raw.split(/\r?\n/)];
  for (const candidate of candidates) {
    const obj = firstJsonObject(candidate.trim());
    if (!obj) continue;
    const msg = obj.message ?? obj.error;
    if (msg != null && String(msg).trim()) return String(msg);
  }
  return null;
}

/**
 * Product status-bar copy: short Chinese, never JSON, never the same
 * sentence in English and Chinese. Use this for injected failure-state
 * tests — do not unmount the RAM disk.
 */
export function userErrorMessage(raw: unknown, letter = 'R'): string {
  let text = raw == null ? '' : String(raw).trim();
  if (!text) return '出错了';
  const extracted = jsonErrorText(text);
  if (extracted) text = extracted.trim();
  const drive = DRIVE_UNMOUNTED.exec(text);
  if (drive) return `${drive[1].toUpperCase()}: 未挂载`;
  const lowered = text.toLowerCase();
  if (lowered.includes('will not create a ram disk') || lowered.includes('is not mounted')) {
    const letterCh = (letter || 'R').replace(/:+$/, '').toUpperCase() || 'R';
    return `${letterCh}: 未挂载`;
  }
  if (text.includes('未找到卷标')) {
    const match = /未找到卷标\s+(\S+)/.exec(text);
    const label = (match?.[1] ?? 'DLSSRAM').replace(/[的。]+$/g, '');
    return `未找到 ${label}`;
  }
  if (lowered.includes('export-dir must be on the working disk')) {
    return '请选择工作盘上的导出目录';
  }
  if (text.trimStart().startsWith('{') && text.includes('"event"')) {
    return '出错了';
  }
  return text.split(/\s+/).join(' ');
}
