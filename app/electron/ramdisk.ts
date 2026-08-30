import {spawn} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

/**
 * GUI-side RAM-disk management (DLSSRAM via ImDisk). TS port of
 * pipeline/ramdisk.py so the packaged app needs no system Python:
 * inspect/render scripts run inside Blender's own interpreter, and the
 * GUI talks to imdisk directly.
 *
 * No fallback filesystem: NTFS via `imdisk -p` needs an elevated token
 * (format.com). If the format fails, report the permission problem —
 * never pretend the disk exists.
 */

export const IMDISK_MISSING = '请安装 ImDisk Toolkit';
export const VOLUME_LABEL = 'DLSSRAM';
export const NTFS_FORMAT_ERROR =
  '内存盘创建失败：NTFS 快速格式化需要管理员权限（format.com）。请以管理员身份运行，或改用缓存路径模式。';

const PREFERRED_LETTERS = 'RSTUVWXYZPQ';
const NTFS_FORMAT_ARGS = '/fs:ntfs /q /y /v:DLSSRAM';
const IMDISK_CANDIDATES = [
  'C:\\Windows\\System32\\imdisk.exe',
  'C:\\Windows\\SysWOW64\\imdisk.exe',
  'C:\\Program Files\\ImDisk\\imdisk.exe',
  'C:\\Program Files (x86)\\ImDisk\\imdisk.exe',
  'C:\\Program Files\\ImDisk Toolkit\\imdisk.exe',
  'C:\\Program Files (x86)\\ImDisk Toolkit\\imdisk.exe',
];

export type VolumeInfo = {
  letter: string;
  label: string;
  filesystem: string;
  totalBytes: number;
  freeBytes: number;
};

export type RamDiskInfo = {
  letter: string;
  root: string;
  filesystem: string;
  label: string;
  totalBytes: number;
  freeBytes: number;
};

export type RamdiskResult<T> = {ok: true; value: T} | {ok: false; error: string; missing?: boolean};

function normalizeLetter(letter: string | null | undefined): string {
  return (letter ?? '').trim().replace(/:$/, '').toUpperCase();
}

function driveExists(letter: string): boolean {
  try {
    return fs.existsSync(`${letter}:\\`);
  } catch {
    return false;
  }
}

function runProcess(
  exe: string,
  args: string[],
  timeoutMs: number
): Promise<{code: number; stdout: string; stderr: string}> {
  return new Promise((resolve, reject) => {
    const proc = spawn(exe, args, {windowsHide: true});
    let stdout = '';
    let stderr = '';
    proc.stdout?.on('data', (chunk: Buffer) => {
      stdout += chunk.toString('utf8');
    });
    proc.stderr?.on('data', (chunk: Buffer) => {
      stderr += chunk.toString('utf8');
    });
    const timer = setTimeout(() => {
      proc.kill();
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

export async function findImdisk(): Promise<string | null> {
  try {
    const where = await runProcess('where.exe', ['imdisk'], 10000);
    const hit = where.stdout
      .split(/\r?\n/)
      .map((line) => line.trim())
      .find((line) => /imdisk\.exe$/i.test(line));
    if (hit) return hit;
  } catch {
    /* not on PATH — try the known install locations */
  }
  for (const candidate of IMDISK_CANDIDATES) {
    try {
      if (fs.statSync(candidate).isFile()) return candidate;
    } catch {
      /* keep looking */
    }
  }
  return null;
}

/** All volumes with a drive letter, in one Get-Volume call. */
export async function listVolumes(timeoutMs = 30000): Promise<VolumeInfo[]> {
  const script =
    '[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; ' +
    'ConvertTo-Json -InputObject @{volumes=@(Get-Volume -ErrorAction SilentlyContinue | ' +
    'Select-Object DriveLetter,FileSystemLabel,FileSystem,Size,SizeRemaining)} -Compress -Depth 4';
  const result = await runProcess(
    'powershell.exe',
    ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', script],
    timeoutMs
  );
  let parsed: {volumes?: unknown} | null = null;
  try {
    parsed = JSON.parse(result.stdout) as {volumes?: unknown};
  } catch {
    parsed = null;
  }
  const rows = Array.isArray(parsed?.volumes) ? (parsed.volumes as Record<string, unknown>[]) : [];
  const volumes: VolumeInfo[] = [];
  for (const row of rows) {
    // PS 5.1 serializes char DriveLetter as its code point (e.g. 82 for R).
    const raw = row.DriveLetter;
    const letter = normalizeLetter(typeof raw === 'number' ? String.fromCharCode(raw) : String(raw ?? ''));
    if (!/^[A-Z]$/.test(letter)) continue;
    volumes.push({
      letter,
      label: typeof row.FileSystemLabel === 'string' ? row.FileSystemLabel : '',
      filesystem: typeof row.FileSystem === 'string' ? row.FileSystem : '',
      totalBytes: typeof row.Size === 'number' && Number.isFinite(row.Size) ? row.Size : 0,
      freeBytes:
        typeof row.SizeRemaining === 'number' && Number.isFinite(row.SizeRemaining) ? row.SizeRemaining : 0,
    });
  }
  return volumes;
}

function pickLetter(
  used: Set<string>,
  requested?: string
): {ok: true; letter: string} | {ok: false; error: string} {
  if (requested) {
    const letter = normalizeLetter(requested);
    if (!/^[A-Z]$/.test(letter)) return {ok: false, error: `invalid drive letter: ${requested}`};
    if (used.has(letter)) return {ok: false, error: `drive letter ${letter}: is already in use`};
    return {ok: true, letter};
  }
  for (const ch of PREFERRED_LETTERS) {
    if (!used.has(ch)) return {ok: true, letter: ch};
  }
  return {ok: false, error: 'no free drive letter for ImDisk RAM disk'};
}

/** Find an already-mounted DLSSRAM disk. Never creates, never unmounts.
 *  With a hint letter, that drive is accepted even if its label differs. */
export async function discoverRamdisk(hint?: string): Promise<RamdiskResult<RamDiskInfo>> {
  const imdisk = await findImdisk();
  if (!imdisk) return {ok: false, error: IMDISK_MISSING, missing: true};
  const volumes = await listVolumes();
  const matches = volumes.filter((v) => v.label.toUpperCase() === VOLUME_LABEL.toUpperCase());
  const wanted = normalizeLetter(hint);
  let chosen: VolumeInfo | null = null;
  if (wanted) {
    const wantedVol = volumes.find((v) => v.letter === wanted);
    if (!wantedVol) return {ok: false, error: `${wanted}: 未挂载`};
    if (wantedVol.label.toUpperCase() === VOLUME_LABEL.toUpperCase()) {
      chosen = wantedVol;
    } else if (matches.length === 0 || matches.some((v) => v.letter === wanted)) {
      chosen = wantedVol;
    } else {
      return {ok: false, error: `${wanted}: 卷标不是 ${VOLUME_LABEL}`};
    }
  } else if (matches.length) {
    chosen = matches.find((v) => v.letter === 'R') ?? matches[0];
  } else {
    return {ok: false, error: `未找到卷标 ${VOLUME_LABEL} 的内存盘`};
  }
  return {
    ok: true,
    value: {
      letter: chosen.letter,
      root: `${chosen.letter}:\\`,
      filesystem: chosen.filesystem,
      label: chosen.label,
      totalBytes: chosen.totalBytes,
      freeBytes: chosen.freeBytes,
    },
  };
}

async function forceRemoveUnitsAt(imdisk: string, letter: string): Promise<void> {
  // `imdisk -l` pairs "Mapping <n>:" headers with a following "Mounting X:".
  const listing = await runProcess(imdisk, ['-l'], 30000);
  const lines = listing.stdout.split(/\r?\n/);
  const units: string[] = [];
  let current: string | null = null;
  for (const line of lines) {
    const header = /^Mapping\s+(\d+):/i.exec(line.trim());
    if (header) {
      current = header[1];
      continue;
    }
    if (current != null && new RegExp(`Mounting\\s+${letter}:`, 'i').test(line)) {
      units.push(current);
    }
    if (/^No Mounting/i.test(line.trim())) current = null;
  }
  for (const unit of units) {
    await runProcess(imdisk, ['-D', '-u', unit], 30000).catch(() => {});
    await runProcess(imdisk, ['-R', '-u', unit], 30000).catch(() => {});
  }
}

export async function unmountRamdisk(letter: string): Promise<RamdiskResult<null>> {
  const imdisk = await findImdisk();
  if (!imdisk) return {ok: false, error: IMDISK_MISSING, missing: true};
  const target = normalizeLetter(letter);
  if (!/^[A-Z]$/.test(target)) return {ok: false, error: `invalid drive letter: ${letter}`};
  try {
    await runProcess(imdisk, ['-d', '-m', `${target}:`], 60000);
    if (driveExists(target)) {
      await runProcess(imdisk, ['-D', '-m', `${target}:`], 60000);
    }
    if (driveExists(target)) {
      await forceRemoveUnitsAt(imdisk, target);
    }
  } catch (err) {
    return {ok: false, error: err instanceof Error ? err.message : String(err)};
  }
  if (driveExists(target)) {
    return {ok: false, error: `ImDisk 未能卸载 ${target}:`};
  }
  return {ok: true, value: null};
}

async function forceDetach(imdisk: string, letter: string): Promise<void> {
  try {
    if (driveExists(letter)) {
      await runProcess(imdisk, ['-D', '-m', `${letter}:`], 60000);
    }
    if (driveExists(letter)) {
      await forceRemoveUnitsAt(imdisk, letter);
    }
  } catch {
    /* best effort cleanup */
  }
}

/** Create an NTFS RAM disk. Needs an elevated token for format.com. */
export async function createRamdisk(size = '8G', requested?: string): Promise<RamdiskResult<RamDiskInfo>> {
  const imdisk = await findImdisk();
  if (!imdisk) return {ok: false, error: IMDISK_MISSING, missing: true};
  const used = new Set((await listVolumes()).map((v) => v.letter));
  const pick = pickLetter(used, requested);
  if (!pick.ok) return {ok: false, error: pick.error};
  const letter = pick.letter;
  const attach = (diskSize: string) =>
    runProcess(
      imdisk,
      ['-a', '-t', 'vm', '-s', diskSize, '-m', `${letter}:`, '-p', NTFS_FORMAT_ARGS],
      120000
    );
  const filesystemNow = async (): Promise<string> => {
    const vol = (await listVolumes()).find((v) => v.letter === letter);
    return vol?.filesystem ?? '';
  };
  try {
    // 64MB probe: proves format.com works before committing to the full disk.
    await attach('64M');
    const probeFs = await filesystemNow();
    await forceDetach(imdisk, letter);
    if (probeFs.toUpperCase() !== 'NTFS') {
      return {ok: false, error: NTFS_FORMAT_ERROR};
    }
    const result = await attach(size);
    const filesystem = await filesystemNow();
    if (result.code !== 0 || filesystem.toUpperCase() !== 'NTFS') {
      await forceDetach(imdisk, letter);
      const log = (result.stderr || result.stdout || '').trim();
      return {ok: false, error: `ImDisk 在 ${letter}: 上创建 ${size} 内存盘失败 ${log.slice(-200)}`.trim()};
    }
    const info = (await listVolumes()).find((v) => v.letter === letter);
    return {
      ok: true,
      value: {
        letter,
        root: `${letter}:\\`,
        filesystem,
        label: info?.label || VOLUME_LABEL,
        totalBytes: info?.totalBytes ?? 0,
        freeBytes: info?.freeBytes ?? 0,
      },
    };
  } catch (err) {
    await forceDetach(imdisk, letter);
    return {ok: false, error: err instanceof Error ? err.message : String(err)};
  }
}
