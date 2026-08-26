import {app} from 'electron';
import fs from 'node:fs/promises';
import fsSync from 'node:fs';
import path from 'node:path';
import {basename, parseDcp, serializeDcp, type DcpProject, type RecentProject} from '../src/dcp';

const RECENT_MAX = 24;

type RecentRecord = {
  path: string;
  openedAt: number;
};

function recentFile() {
  return path.join(app.getPath('userData'), 'recent.json');
}

async function readRecentRecords(): Promise<RecentRecord[]> {
  try {
    const raw = await fs.readFile(recentFile(), 'utf8');
    const parsed = JSON.parse(raw) as unknown;
    if (!Array.isArray(parsed)) return [];
    return parsed
      .map((item) => {
        if (!item || typeof item !== 'object') return null;
        const rec = item as Record<string, unknown>;
        if (typeof rec.path !== 'string' || !rec.path) return null;
        return {path: rec.path, openedAt: typeof rec.openedAt === 'number' ? rec.openedAt : 0};
      })
      .filter((item): item is RecentRecord => Boolean(item));
  } catch {
    return [];
  }
}

export async function listRecent(): Promise<RecentProject[]> {
  const records = await readRecentRecords();
  const out: RecentProject[] = [];
  for (const rec of records) {
    if (!fsSync.existsSync(rec.path)) continue;
    try {
      const raw = await fs.readFile(rec.path, 'utf8');
      const parsed = parseDcp(raw);
      const st = await fs.stat(rec.path);
      out.push({
        path: rec.path,
        name: projectName(rec.path),
        mode: parsed.ok ? parsed.project.mode : '',
        mtime: st.mtimeMs,
        preview: parsed.ok ? parsed.project.preview || null : null
      });
    } catch {
      /* skip unreadable */
    }
  }
  return out;
}

export async function rememberRecent(dcpPath: string): Promise<RecentProject[]> {
  const abs = path.resolve(dcpPath);
  const records = (await readRecentRecords()).filter((item) => path.resolve(item.path) !== abs);
  records.unshift({path: abs, openedAt: Date.now()});
  await fs.mkdir(path.dirname(recentFile()), {recursive: true});
  await fs.writeFile(recentFile(), `${JSON.stringify(records.slice(0, RECENT_MAX), null, 2)}\n`);
  return listRecent();
}

export async function loadDcpFile(dcpPath: string): Promise<{ok: boolean; project?: DcpProject; error?: string}> {
  try {
    const raw = await fs.readFile(dcpPath, 'utf8');
    const parsed = parseDcp(raw);
    if (!parsed.ok) return {ok: false, error: parsed.error};
    return {ok: true, project: parsed.project};
  } catch (err) {
    return {ok: false, error: err instanceof Error ? err.message : String(err)};
  }
}

export async function saveDcpFile(
  dcpPath: string,
  project: DcpProject
): Promise<{ok: boolean; error?: string}> {
  try {
    const parsed = parseDcp(serializeDcp(project));
    if (!parsed.ok) return {ok: false, error: parsed.error};
    await fs.mkdir(path.dirname(dcpPath), {recursive: true});
    await fs.writeFile(dcpPath, serializeDcp(parsed.project));
    return {ok: true};
  } catch (err) {
    return {ok: false, error: err instanceof Error ? err.message : String(err)};
  }
}

export function fileExists(filePath: string) {
  try {
    return Boolean(filePath) && fsSync.existsSync(filePath);
  } catch {
    return false;
  }
}

export function ensureDcpExtension(filePath: string) {
  return filePath.toLowerCase().endsWith('.dcp') ? filePath : `${filePath}.dcp`;
}

function projectName(dcpPath: string) {
  return basename(dcpPath).replace(/\.dcp$/i, '') || basename(dcpPath);
}
