import {createContext, useCallback, useContext, useEffect, useMemo, useState, type ReactNode} from 'react';
import {userErrorMessage} from './errors';

const PATHS_KEY = 'dc-nr-paths';

export type StoredPaths = {colorSource: string; bufferDir: string};

function loadPaths(): StoredPaths {
  try {
    const raw = window.localStorage.getItem(PATHS_KEY);
    if (!raw) return {colorSource: '', bufferDir: ''};
    return {colorSource: '', bufferDir: '', ...(JSON.parse(raw) as Partial<StoredPaths>)};
  } catch {
    return {colorSource: '', bufferDir: ''};
  }
}

/**
 * NR 工具的会话核心：路径、打开的会话、导出状态。放在页面外的 context，
 * 因为「打开」按钮按 Spectrum 的页面主操作约定放在标题栏（App.tsx），
 * 而预览泵、调参、导出表单仍留在 NrToolPage。
 */
type NrSessionStore = {
  paths: StoredPaths;
  setPaths: (updater: (prev: StoredPaths) => StoredPaths) => void;
  info: NrToolInfo | null;
  opening: boolean;
  exporting: boolean;
  exportPercent: number;
  message: string;
  error: string | null;
  setMessage: (value: string) => void;
  setError: (value: string | null) => void;
  openSession: () => Promise<void>;
  /** 离开页面时释放 CLI 侧的 NR 会话（feature + 显存纹理）。 */
  closeSession: () => void;
};

const NrSessionContext = createContext<NrSessionStore | null>(null);

export function NrSessionProvider({children}: {children: ReactNode}) {
  const [paths, setPathsState] = useState<StoredPaths>(loadPaths);
  const [info, setInfo] = useState<NrToolInfo | null>(null);
  const [opening, setOpening] = useState(false);
  const [exporting, setExporting] = useState(false);
  const [exportPercent, setExportPercent] = useState(0);
  const [message, setMessage] = useState('打开已导出的视频或序列帧 + dlssnrbuffer，预览调整 NR');
  const [error, setError] = useState(null as string | null);

  // Persist paths so re-entering the tool restores the workspace.
  useEffect(() => {
    try {
      window.localStorage.setItem(PATHS_KEY, JSON.stringify(paths));
    } catch {
      /* private mode */
    }
  }, [paths]);

  // NR export progress arrives over the shared orchestrator event channel.
  useEffect(
    () =>
      window.dlss.onEvent((event) => {
        if (event.type === 'nrtool_progress' && typeof event.percent === 'number') {
          setExportPercent(Math.min(100, Math.round(event.percent * 100)));
        } else if (event.type === 'nrtool_export_done') {
          setExporting(false);
          setExportPercent(100);
          setMessage('NR 导出完成');
        } else if (event.type === 'nrtool_export_stopped') {
          setExporting(false);
          setMessage('已停止');
        } else if (event.type === 'nrtool_export_error') {
          setExporting(false);
          setError(userErrorMessage(String(event.message ?? 'NR 导出失败')));
        }
      }),
    []
  );

  const openSession = useCallback(async () => {
    if (!paths.colorSource.trim()) {
      setError('请选择色彩源');
      return;
    }
    setOpening(true);
    setError(null);
    setInfo(null);
    try {
      const params: {color_source: string; buffer_dir?: string} = {
        color_source: paths.colorSource.trim()
      };
      // 无 buffer 也能开：仅引导模式「无」可用（CLI 侧同样强制）。
      if (paths.bufferDir.trim()) params.buffer_dir = paths.bufferDir.trim();
      const result = await window.dlss.nrOpen(params);
      if (!result.ok) {
        setError(userErrorMessage(result.error ?? '打开失败'));
        return;
      }
      setInfo(result);
      setMessage(
        `${result.frames} 帧 · ${result.width}×${result.height} · ${
          result.color_kind === 'video' ? '视频' : 'PNG 序列'
        }`
      );
    } catch (err) {
      setError(userErrorMessage(err instanceof Error ? err.message : '打开失败'));
    } finally {
      setOpening(false);
    }
  }, [paths]);

  const closeSession = useCallback(() => {
    void window.dlss.nrClose();
    setInfo(null);
    setExporting(false);
  }, []);

  const value = useMemo(
    () => ({
      paths,
      setPaths: setPathsState,
      info,
      opening,
      exporting,
      exportPercent,
      message,
      error,
      setMessage,
      setError,
      openSession,
      closeSession
    }),
    [paths, info, opening, exporting, exportPercent, message, error, openSession, closeSession]
  );

  return <NrSessionContext.Provider value={value}>{children}</NrSessionContext.Provider>;
}

export function useNrSession(): NrSessionStore {
  const store = useContext(NrSessionContext);
  if (!store) throw new Error('useNrSession outside NrSessionProvider');
  return store;
}
