import {createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode} from 'react';
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
 * NR/FG 工具的会话核心：路径、打开的会话、导出状态。放在页面外的 context，
 * 因为导出/停止按钮按 Spectrum 的页面主操作约定放在标题栏（App.tsx），
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
  /** 清空色彩源时释放 CLI 会话。切页不关闭会话。 */
  closeSession: () => void;
  stopExport: () => Promise<void>;
  /** 导出发起前锁定 UI（标题栏停止、进度条交换）；RPC 失败用 endExport 回滚。 */
  beginExport: () => boolean;
  endExport: () => void;
  isExporting: () => boolean;
  /** 标题栏「导出」按钮触发页面里组装好参数的 startExport。 */
  exportTrigger: (() => void) | null;
  setExportTrigger: (fn: (() => void) | null) => void;
};

const NrSessionContext = createContext<NrSessionStore | null>(null);

export function NrSessionProvider({children}: {children: ReactNode}) {
  const [paths, setPathsState] = useState<StoredPaths>(loadPaths);
  const [info, setInfo] = useState<NrToolInfo | null>(null);
  const [opening, setOpening] = useState(false);
  const openingRef = useRef(false);
  const [exporting, setExporting] = useState(false);
  const exportingRef = useRef(false);
  const [exportPercent, setExportPercent] = useState(0);
  const [message, setMessage] = useState('选择色彩源后自动打开会话，预览调整 NR / FG');
  const [error, setError] = useState(null as string | null);
  const [exportTrigger, setExportTriggerState] = useState<(() => void) | null>(null);
  const setExportTrigger = useCallback((trigger: (() => void) | null) => {
    setExportTriggerState(() => trigger);
  }, []);
  const isExporting = useCallback(() => exportingRef.current, []);
  const endExport = useCallback(() => {
    exportingRef.current = false;
    setExporting(false);
  }, []);

  // Persist paths so re-entering the tool restores the workspace.
  useEffect(() => {
    try {
      window.localStorage.setItem(PATHS_KEY, JSON.stringify(paths));
    } catch {
      /* private mode */
    }
  }, [paths]);

  // NR/FG export progress arrives over the shared orchestrator event channel.
  useEffect(
    () =>
      window.dlss.onEvent((event) => {
        if (!exportingRef.current) return;
        if (event.type === 'nrtool_progress' && typeof event.percent === 'number') {
          // 进度只进不退：任何上游双源/乱序都不该把条往回拽。
          const pct = Math.min(100, Math.round(event.percent * 100));
          setExportPercent((prev) => Math.max(prev, pct));
        } else if (event.type === 'nrtool_export_done') {
          endExport();
          setExportPercent(100);
          setMessage('导出完成');
        } else if (event.type === 'nrtool_export_stopped') {
          endExport();
          setMessage('已停止');
        } else if (event.type === 'nrtool_export_error') {
          endExport();
          setError(userErrorMessage(String(event.message ?? '导出失败')));
        }
      }),
    [endExport]
  );

  const openSession = useCallback(async () => {
    if (openingRef.current || exportingRef.current) return;
    if (!paths.colorSource.trim()) {
      setError('请选择色彩源');
      return;
    }
    openingRef.current = true;
    setOpening(true);
    setError(null);
    setInfo(null);
    try {
      const params: {color_source: string; buffer_dir?: string; camera_config?: string} = {
        color_source: paths.colorSource.trim()
      };
      // 无 buffer 也能开：仅引导模式「无」+ FG 不可用（CLI 侧同样强制）。
      const bufferDir = paths.bufferDir.trim();
      if (bufferDir) {
        params.buffer_dir = bufferDir;
        // FG 的逐帧相机数据由导出任务拷进缓冲目录；存在才带上。
        const cfgPath = `${bufferDir.replace(/[\\/]+$/, '')}\\dlssconfig.json`;
        try {
          if (await window.dlss.exists(cfgPath)) params.camera_config = cfgPath;
        } catch {
          /* probe failure → omit; FG errors loud at eval time */
        }
      }
      const result = await window.dlss.nrOpen(params);
      if (!result.ok) {
        setError(userErrorMessage(result.error ?? '打开失败'));
        return;
      }
      setInfo(result);
      setMessage(
        `${result.frames} 帧 · ${result.width}×${result.height} · ${
          result.color_kind === 'video' ? '视频' : 'PNG 序列'
        }${result.buffer_width > 0 ? ' · 有引导缓冲' : ''}`
      );
    } catch (err) {
      setError(userErrorMessage(err instanceof Error ? err.message : '打开失败'));
    } finally {
      openingRef.current = false;
      setOpening(false);
    }
  }, [paths]);

  const closeSession = useCallback(() => {
    if (openingRef.current || exportingRef.current) return;
    void window.dlss.nrClose().catch((err: unknown) => {
      setError(userErrorMessage(err instanceof Error ? err.message : '关闭失败'));
    });
    setInfo(null);
    setError(null);
    setMessage('选择色彩源后自动打开会话，预览调整 NR / FG');
  }, []);

  const stopExport = useCallback(async () => {
    if (!exportingRef.current) return;
    try {
      const result = await window.dlss.nrStop();
      if (!result.ok) setError(userErrorMessage(result.error ?? '停止失败'));
    } catch (err) {
      setError(userErrorMessage(err instanceof Error ? err.message : '停止失败'));
    }
  }, []);

  const beginExport = useCallback(() => {
    if (openingRef.current || exportingRef.current) return false;
    exportingRef.current = true;
    setExporting(true);
    setExportPercent(0);
    setError(null);
    setMessage('准备导出…');
    return true;
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
      closeSession,
      stopExport,
      beginExport,
      endExport,
      isExporting,
      exportTrigger,
      setExportTrigger
    }),
    [
      paths,
      info,
      opening,
      exporting,
      exportPercent,
      message,
      error,
      openSession,
      closeSession,
      stopExport,
      beginExport,
      endExport,
      isExporting,
      exportTrigger,
      setExportTrigger
    ]
  );

  return <NrSessionContext.Provider value={value}>{children}</NrSessionContext.Provider>;
}

export function useNrSession(): NrSessionStore {
  const store = useContext(NrSessionContext);
  if (!store) throw new Error('useNrSession outside NrSessionProvider');
  return store;
}
