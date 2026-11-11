import {createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode} from 'react';
import {
  emptyFrameTiming,
  exportProgressRatio,
  formatEtaLine,
  jobCount,
  mapBlenderFrame,
  recordFrameAdvance,
  startFrameTiming,
  type FrameTiming
} from './progress';
import {
  DEFAULT_FILENAME_TEMPLATE,
  DEFAULT_SPLITTERS,
  EMPTY_PINS,
  RECOMMENDED_PERCENT,
  defaultDcpPath,
  emptyProject,
  fgAllowed,
  jobTokenFromBlend,
  nativeResAllowed,
  nextOutputVersion,
  outputFrameCount,
  parseFg,
  projectTitle,
  resolveFilenameTemplate,
  rrAllowed,
  rrQualityAllowed,
  type DcpProject,
  type JobRecord,
  type PinKey,
  type Pins,
  type RecentProject
} from './dcp';
import {userErrorMessage} from './errors';

export type Route = 'project' | 'export' | 'jobs' | 'nrtool' | 'settings';
export type ColorSchemePref = 'dark' | 'light' | 'system';
export type JobStatus = 'idle' | 'running' | 'done' | 'error';

export type QueueJob = {
  id: number;
  name: string;
  status: JobStatus;
  frameStart: number;
  frameEnd: number;
  outputWidth: number;
  outputHeight: number;
  mode: string;
  outputDir: string;
  outputFormat: string;
  template: string;
};

export const ROUTE_LABELS: Record<Route, string> = {
  project: '项目',
  export: '导出',
  jobs: 'Jobs',
  nrtool: 'NR / FG',
  settings: '设置'
};

const COLOR_SCHEME_KEY = 'dc-color-scheme';
const CLI_LOG_KEY = 'dc-cli-log';

export function padFrame(n: number) {
  return String(n).padStart(4, '0');
}

export function basename(filePath: string) {
  return filePath.replace(/\\/g, '/').split('/').filter(Boolean).pop() || filePath;
}

export function statusLabel(status: JobStatus) {
  switch (status) {
    case 'running':
      return '导出中';
    case 'done':
      return '完成';
    case 'error':
      return '错误';
    default:
      return '就绪';
  }
}

export function engineLabel(engine: string) {
  if (engine === 'CYCLES') return 'Cycles';
  if (engine.includes('EEVEE')) return 'EEVEE';
  return engine || '未知';
}

function ngxMinSize(outputW: number, outputH: number, quality: string) {
  const scale = quality === 'ULTRA_PERFORMANCE' ? 3 : 2;
  // NGX reports a CEILed floor for odd sizes (1215→608, measured 2026-08-31).
  return [Math.max(1, Math.ceil(outputW / scale)), Math.max(1, Math.ceil(outputH / scale))] as const;
}

export function percentError(outputW: number, outputH: number, percentage: number, quality: string) {
  const renderW = Math.max(1, Math.floor((outputW * percentage) / 100));
  const renderH = Math.max(1, Math.floor((outputH * percentage) / 100));
  const [minW, minH] = ngxMinSize(outputW, outputH, quality);
  if (renderW < minW || renderH < minH) {
    const rec = RECOMMENDED_PERCENT[quality] ?? 58;
    return `渲染 ${renderW}×${renderH} 低于 NGX ${quality} 下限 ${minW}×${minH}。请约 ${rec}%。`;
  }
  return null;
}

export function useAppState() {
  const [route, setRouteState] = useState<Route>('project');
  const routeRef = useRef<Route>('project');
  const routeHistory = useRef<Route[]>([]);
  const setRoute = useCallback((next: Route) => {
    if (next === routeRef.current) return;
    routeHistory.current.push(routeRef.current);
    routeRef.current = next;
    setRouteState(next);
  }, []);
  const navigateBack = useCallback(() => {
    const previous = routeHistory.current.pop() ?? 'project';
    routeRef.current = previous;
    setRouteState(previous);
  }, []);
  const [colorScheme, setColorSchemeState] = useState<ColorSchemePref>('system');
  const [dcpPath, setDcpPath] = useState(null as string | null);
  const [recents, setRecents] = useState<RecentProject[]>([]);
  const [filenameTemplate, setFilenameTemplate] = useState(DEFAULT_FILENAME_TEMPLATE);
  const [outputVersion, setOutputVersion] = useState(1);
  const [history, setHistory] = useState<JobRecord[]>([]);
  const [projectReady, setProjectReady] = useState(false);
  const [maximized, setMaximized] = useState(false);
  const [blend, setBlend] = useState('');
  const [scene, setScene] = useState(null as SceneState | null);
  const [inspecting, setInspecting] = useState(false);
  const [inspectError, setInspectError] = useState(null as string | null);
  const [pins, setPins] = useState<Pins>(EMPTY_PINS);
  const [mode, setMode] = useState('sr');
  const [quality, setQuality] = useState('BALANCED');
  const [percentage, setPercentage] = useState(100);
  const [frameStart, setFrameStart] = useState(1);
  const [frameEnd, setFrameEnd] = useState(1);
  const [fps, setFps] = useState(24);
  const [outputWidth, setOutputWidth] = useState(1920);
  const [outputHeight, setOutputHeight] = useState(1080);
  const [outputFormat, setOutputFormat] = useState('prores_422hq');
  const [exportDir, setExportDir] = useState('build/stage4-out');
  const [adaptive, setAdaptive] = useState(true);
  const [noiseThreshold, setNoiseThreshold] = useState(0.01);
  const [samples, setSamples] = useState(0);
  const [minSamples, setMinSamples] = useState(0);
  const [timeLimit, setTimeLimit] = useState(0);
  const [useDenoising, setUseDenoising] = useState(false);
  const [denoiser, setDenoiser] = useState('OPENIMAGEDENOISE');
  const [windowSize, setWindowSize] = useState(6);
  const [useRamdisk, setUseRamdisk] = useState(true);
  const [ramdiskGb, setRamdiskGb] = useState(2);
  const [cachePath, setCachePath] = useState('');
  const [elevated, setElevated] = useState(false);
  const [cliLog, setCliLogState] = useState(true);
  const [nativeResBuffers, setNativeResBuffers] = useState(false);
  const [nrBuffer, setNrBuffer] = useState(false);
  // 性能兜底（inject.py 同款默认）：边距不勾 = 沿用 blend 值。
  const [perfPersistent, setPerfPersistent] = useState(true);
  const [perfCameraCull, setPerfCameraCull] = useState(true);
  const [perfCameraCullMarginOverride, setPerfCameraCullMarginOverride] = useState(false);
  const [perfCameraCullMargin, setPerfCameraCullMargin] = useState(0.1);
  const [perfDistanceCull, setPerfDistanceCull] = useState(true);
  const [perfDistanceCullMarginOverride, setPerfDistanceCullMarginOverride] = useState(false);
  const [perfDistanceCullMargin, setPerfDistanceCullMargin] = useState(10);
  const [perfTileSize, setPerfTileSize] = useState(4096);
  // 滚动并行：0 = 关；2..6 = Blender 进程数。
  const [parallelSegments, setParallelSegments] = useState(0);
  const [fg, setFg] = useState(0);
  const [status, setStatus] = useState<JobStatus>('idle');
  /** 导出进度分段：准备（写 dlssconfig）→ 帧导出 → 落盘（mov 收尾）。 */
  const [exportPhase, setExportPhase] = useState<'prepare' | 'render' | 'finalize' | null>(null);
  /** bake（写 dlssconfig）段内比例；渲染开始后 prepare 段视为完成。 */
  const [prepareRatio, setPrepareRatio] = useState(0);
  // Export progress belongs to the job, never to the editable Blender timeline.
  const [renderRatio, setRenderRatio] = useState(0);
  const [currentFrame, setCurrentFrame] = useState(1);
  const previewCanvasRef = useRef<HTMLCanvasElement | null>(null);
  const [previewAvailable, setPreviewAvailable] = useState(false);
  const previewBitmapRef = useRef<ImageBitmap | null>(null);
  const pendingPreviewRef = useRef<{b64: string; resizeW: number} | null>(null);
  const previewPumpActiveRef = useRef(false);
  const [probe, setProbe] = useState(null as ProbeResult | null);
  const [message, setMessage] = useState('就绪');
  const [error, setError] = useState(null as string | null);
  const skipAutosave = useRef(false);
  const skipRemoteInspect = useRef(false);
  const [jobs, setJobs] = useState<QueueJob[]>([]);
  const jobSeq = useRef(1);
  const activeJobIdRef = useRef<number | null>(null);
  const activeJobRef = useRef<QueueJob | null>(null);
  const playheadRef = useRef(1);
  const [frameTiming, setFrameTiming] = useState<FrameTiming>(emptyFrameTiming);

  const frameCount = jobCount(frameStart, frameEnd);
  const ngxWarning = percentError(outputWidth, outputHeight, percentage, quality);
  const busy = status === 'running';
  const jobName = blend ? basename(blend) : '未选择 blend';
  const hasProject = Boolean(dcpPath);
  const hasScene = Boolean(scene?.ok);
  const samplingLocked = !hasScene || !scene?.cycles;
  const canRR = !hasScene || rrAllowed(scene?.engine);
  const nativeResEnabled = hasScene && nativeResAllowed(scene?.engine, mode);
  const fgEnabled = fgAllowed(mode);
  // The guide video is written at input rate — mutually exclusive with FG.
  const fgMultiplier = fgEnabled && !nrBuffer ? parseFg(fg) : 0;
  const rrLocksDenoise = mode === 'rr';
  const outputFrames = outputFrameCount(frameCount, fgMultiplier);
  const displayFrame = Math.min(frameEnd, Math.max(frameStart, currentFrame));
  const playhead = exportProgressRatio(status, renderRatio);
  const progressPercent = Math.round(playhead * 100);
  const remainingFrames = Math.max(0, frameEnd - displayFrame + 1);
  /** 发起任务前置校验：场景就绪 + 输入缓冲选好（内存盘需管理员，路径模式需缓存路径）。 */
  const bufferReady = useRamdisk && elevated ? true : cachePath.trim() !== '';
  const canStart = hasScene && !inspecting && bufferReady;
  const etaText =
    busy && !error ? formatEtaLine(frameTiming, remainingFrames) : null;

  function setColorScheme(next: ColorSchemePref) {
    setColorSchemeState(next);
    try {
      window.localStorage.setItem(COLOR_SCHEME_KEY, next);
    } catch {
      /* private mode etc. */
    }
  }

  /** CLI 调试日志开关：dev 默认开、打包版默认关（main 侧 isPackaged），
   *  用户改选持久化到 localStorage 并即时推给 orchestrator。 */
  function setCliLog(next: boolean) {
    setCliLogState(next);
    try {
      window.localStorage.setItem(CLI_LOG_KEY, next ? 'on' : 'off');
    } catch {
      /* private mode etc. */
    }
    void window.dlss.setCliLog(next);
  }

  function setPlayhead(next: number) {
    playheadRef.current = next;
    setCurrentFrame(next);
  }

  function resetFrameTiming(nextFrame = frameStart) {
    playheadRef.current = nextFrame;
    setCurrentFrame(nextFrame);
    setFrameTiming(startFrameTiming(Date.now()));
  }

  function clearFrameTiming() {
    setFrameTiming(emptyFrameTiming());
  }

  function advancePlayhead(mapped: number) {
    const prev = playheadRef.current;
    const next = Math.max(prev, mapped);
    if (next <= prev) return;
    playheadRef.current = next;
    setCurrentFrame(next);
    setFrameTiming((timing) => recordFrameAdvance(timing, prev, next, Date.now()));
  }

  /**
   * Latest-wins preview decoding: the CLI hands out full-resolution jpegs and the
   * poll delivers them faster than decoding 4K bitmaps makes sense. Only the newest
   * frame is decoded, downscaled to the display size, and drawn into the canvas;
   * the previous bitmap is closed so nothing accumulates.
   */
  const drawPreviewBitmap = useCallback(async (job: {b64: string; resizeW: number}) => {
    try {
      const canvas = previewCanvasRef.current;
      if (!canvas) return;
      const binary = atob(job.b64);
      const bytes = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
      const options: ImageBitmapOptions | undefined =
        job.resizeW > 0 ? {resizeWidth: job.resizeW, resizeQuality: 'medium'} : undefined;
      const bitmap = await createImageBitmap(new Blob([bytes], {type: 'image/jpeg'}), options);
      const target = previewCanvasRef.current;
      if (!target) {
        bitmap.close();
        return;
      }
      target.width = bitmap.width;
      target.height = bitmap.height;
      target.getContext('2d')?.drawImage(bitmap, 0, 0);
      previewBitmapRef.current?.close();
      previewBitmapRef.current = bitmap;
      setPreviewAvailable(true);
    } catch {
      /* malformed jpeg */
    }
  }, []);

  const pumpPreview = useCallback(() => {
    if (previewPumpActiveRef.current) return;
    previewPumpActiveRef.current = true;
    const tick = () => {
      const next = pendingPreviewRef.current;
      pendingPreviewRef.current = null;
      if (next == null) {
        previewPumpActiveRef.current = false;
        return;
      }
      void drawPreviewBitmap(next).finally(() => window.setTimeout(tick, 120));
    };
    window.setTimeout(tick, 0);
  }, [drawPreviewBitmap]);

  const registerPreviewCanvas = useCallback(
    (canvas: HTMLCanvasElement | null) => {
      previewCanvasRef.current = canvas;
      if (canvas != null) {
        const bitmap = previewBitmapRef.current;
        if (bitmap) {
          canvas.width = bitmap.width;
          canvas.height = bitmap.height;
          canvas.getContext('2d')?.drawImage(bitmap, 0, 0);
        }
        pumpPreview();
      }
    },
    [pumpPreview]
  );

  const clearPreviewCanvas = useCallback(() => {
    setPreviewAvailable(false);
    pendingPreviewRef.current = null;
    const canvas = previewCanvasRef.current;
    if (canvas) {
      canvas.width = 0;
      canvas.height = 0;
    }
    previewBitmapRef.current?.close();
    previewBitmapRef.current = null;
  }, []);

  useEffect(
    () => () => {
      pendingPreviewRef.current = null;
      previewBitmapRef.current?.close();
      previewBitmapRef.current = null;
    },
    []
  );

  const request = useMemo<ExportRequest>(() => {
    const overrides: SceneOverrides = {};
    if (pins.percentage) overrides.percentage = percentage;
    if (pins.frameStart) overrides.frameStart = frameStart;
    if (pins.frameEnd) overrides.frameEnd = frameEnd;
    if (pins.adaptive) overrides.useAdaptiveSampling = adaptive;
    if (pins.noiseThreshold) overrides.adaptiveThreshold = noiseThreshold;
    if (pins.samples) overrides.samples = samples;
    if (pins.minSamples) overrides.adaptiveMinSamples = minSamples;
    if (pins.timeLimit) overrides.timeLimit = timeLimit;
    if (!rrLocksDenoise && pins.denoise) {
      overrides.useDenoising = useDenoising;
      if (useDenoising) overrides.denoiser = denoiser;
    }
    return {
      blend,
      mode,
      quality,
      frameStart,
      frameEnd,
      outputWidth,
      outputHeight,
      outputFormat,
      exportDir,
      window: windowSize,
      useRamdisk: useRamdisk && elevated,
      cachePath: useRamdisk && elevated ? '' : cachePath,
      ramdiskGb,
      overrides,
      nativeResBuffers: nativeResEnabled && nativeResBuffers,
      nrBuffer,
      perfPersistent,
      perfCameraCull,
      cameraCullMargin:
        perfCameraCull && perfCameraCullMarginOverride ? perfCameraCullMargin : undefined,
      perfDistanceCull,
      distanceCullMargin:
        perfDistanceCull && perfDistanceCullMarginOverride ? perfDistanceCullMargin : undefined,
      perfTileSize,
      parallelSegments: parallelSegments >= 2 ? Math.min(6, parallelSegments) : undefined,
      fg: fgMultiplier,
      effectivePercentage: pins.percentage ? percentage : (scene?.resolution_percentage ?? percentage)
    };
  }, [
    blend,
    mode,
    quality,
    pins,
    percentage,
    frameStart,
    frameEnd,
    adaptive,
    noiseThreshold,
    samples,
    minSamples,
    timeLimit,
    useDenoising,
    denoiser,
    rrLocksDenoise,
    outputWidth,
    outputHeight,
    outputFormat,
    exportDir,
    windowSize,
    useRamdisk,
    ramdiskGb,
    cachePath,
    nativeResBuffers,
    nativeResEnabled,
    nrBuffer,
    perfPersistent,
    perfCameraCull,
    perfCameraCullMarginOverride,
    perfCameraCullMargin,
    perfDistanceCull,
    perfDistanceCullMarginOverride,
    perfDistanceCullMargin,
    perfTileSize,
    parallelSegments,
    fgMultiplier,
    scene
  ]);

  function snapshotProject(): DcpProject {
    return {
      kind: 'dlss-compositor-project',
      schema: 1,
      blend,
      mode: mode === 'rr' ? 'rr' : 'sr',
      pins,
      percentage,
      frameStart,
      frameEnd,
      fps,
      adaptive,
      noiseThreshold,
      samples,
      minSamples,
      timeLimit,
      useDenoising,
      denoiser,
      quality,
      outputWidth,
      outputHeight,
      outputFormat,
      exportDir,
      filenameTemplate: filenameTemplate || DEFAULT_FILENAME_TEMPLATE,
      outputVersion,
      windowSize,
      useRamdisk,
      ramdiskGb,
      cachePath,
      nativeResBuffers,
      nrBuffer,
      perfPersistent,
      perfCameraCull,
      perfCameraCullMarginOverride,
      perfCameraCullMargin,
      perfDistanceCull,
      perfDistanceCullMarginOverride,
      perfDistanceCullMargin,
      perfTileSize,
      parallelSegments,
      fg: fgMultiplier,
      splitters: {...DEFAULT_SPLITTERS},
      history: history.length ? history : undefined
    };
  }

  function applyProject(project: DcpProject, nextScene: SceneState | null) {
    setBlend(project.blend);
    const nextMode = nextScene && !rrAllowed(nextScene.engine) && project.mode === 'rr' ? 'sr' : project.mode;
    setMode(nextMode);
    setPins(project.pins);
    setPercentage(project.percentage);
    setFrameStart(project.frameStart);
    setFrameEnd(project.frameEnd);
    setFps(project.fps);
    setAdaptive(project.adaptive);
    setNoiseThreshold(project.noiseThreshold);
    setSamples(project.samples);
    setMinSamples(project.minSamples);
    setTimeLimit(project.timeLimit);
    setUseDenoising(project.useDenoising);
    setDenoiser(project.denoiser);
    setQuality(project.quality);
    setOutputWidth(project.outputWidth);
    setOutputHeight(project.outputHeight);
    setOutputFormat(project.outputFormat);
    setExportDir(project.exportDir);
    setFilenameTemplate(project.filenameTemplate || DEFAULT_FILENAME_TEMPLATE);
    setOutputVersion(Math.max(1, project.outputVersion || 1));
    setWindowSize(project.windowSize);
    setUseRamdisk(project.useRamdisk);
    setRamdiskGb(Math.min(64, Math.max(1, Math.round(project.ramdiskGb ?? 2))));
    setCachePath(project.cachePath);
    setNativeResBuffers(Boolean(project.nativeResBuffers));
    setNrBuffer(Boolean(project.nrBuffer));
    setPerfPersistent(project.perfPersistent ?? true);
    setPerfCameraCull(project.perfCameraCull ?? true);
    setPerfCameraCullMarginOverride(Boolean(project.perfCameraCullMarginOverride));
    setPerfCameraCullMargin(project.perfCameraCullMargin ?? 0.1);
    setPerfDistanceCull(project.perfDistanceCull ?? true);
    setPerfDistanceCullMarginOverride(Boolean(project.perfDistanceCullMarginOverride));
    setPerfDistanceCullMargin(project.perfDistanceCullMargin ?? 10);
    setPerfTileSize(project.perfTileSize ?? 4096);
    setParallelSegments(Math.min(6, Math.max(0, Math.trunc(project.parallelSegments ?? 0))));
    setFg(fgAllowed(nextMode) ? parseFg(project.fg) : 0);
    setHistory(project.history ?? []);
    playheadRef.current = project.frameStart;
    setCurrentFrame(project.frameStart);
  }

  function applyInspectedScene(next: SceneState, fromProject?: DcpProject) {
    setInspecting(false);
    if (!next.ok) {
      setScene(null);
      setInspectError(next.error || '读取场景失败');
      setMessage(next.error || '读取场景失败');
      if (fromProject) applyProject(fromProject, null);
      return;
    }
    if (String(next.engine || '').toUpperCase() === 'CYCLES') {
      if (!next.cycles) {
        setScene(null);
        setInspectError('Cycles 场景缺少 cycles 数据');
        setMessage('Cycles 场景缺少 cycles 数据');
        return;
      }
      if (!next.cycles.denoiser) {
        setScene(null);
        setInspectError('Cycles 场景缺少 denoiser');
        setMessage('Cycles 场景缺少 denoiser');
        return;
      }
    }
    setScene(next);
    setInspectError(null);
    if (fromProject) {
      applyProject(fromProject, next);
    } else {
      setPins(EMPTY_PINS);
      setPercentage(next.resolution_percentage);
      setFrameStart(next.frame_start);
      setFrameEnd(next.frame_end);
      setFps(next.fps || 24);
      setOutputWidth(next.resolution_x);
      setOutputHeight(next.resolution_y);
      playheadRef.current = next.frame_start;
      setCurrentFrame(next.frame_start);
      if (next.cycles) {
        setAdaptive(next.cycles.use_adaptive_sampling);
        setNoiseThreshold(next.cycles.adaptive_threshold);
        setSamples(next.cycles.samples);
        setMinSamples(next.cycles.adaptive_min_samples);
        setTimeLimit(next.cycles.time_limit);
        setUseDenoising(next.cycles.use_denoising);
        setDenoiser(next.cycles.denoiser);
      }
      if (!rrAllowed(next.engine) && mode === 'rr') setMode('sr');
    }
    setMessage('就绪');
    setProjectReady(true);
  }

  function restorePin(key: PinKey) {
    if (!scene) return;
    switch (key) {
      case 'percentage':
        setPercentage(scene.resolution_percentage);
        break;
      case 'frameStart':
        setFrameStart(scene.frame_start);
        break;
      case 'frameEnd':
        setFrameEnd(scene.frame_end);
        break;
      case 'adaptive':
        setAdaptive(Boolean(scene.cycles?.use_adaptive_sampling));
        break;
      case 'noiseThreshold':
        setNoiseThreshold(scene.cycles?.adaptive_threshold ?? 0);
        break;
      case 'samples':
        setSamples(scene.cycles?.samples ?? 0);
        break;
      case 'minSamples':
        setMinSamples(scene.cycles?.adaptive_min_samples ?? 0);
        break;
      case 'timeLimit':
        setTimeLimit(scene.cycles?.time_limit ?? 0);
        break;
      case 'denoise':
        setUseDenoising(Boolean(scene.cycles?.use_denoising));
        if (scene.cycles?.denoiser) setDenoiser(scene.cycles.denoiser);
        break;
      default:
        break;
    }
  }

  function setPin(key: PinKey, checked: boolean) {
    setPins((prev) => ({...prev, [key]: checked}));
    if (!checked) restorePin(key);
  }

  async function inspectBlend(filePath: string, fromProject?: DcpProject) {
    skipRemoteInspect.current = true;
    setInspecting(true);
    setInspectError(null);
    setScene(null);
    if (!fromProject) setPins(EMPTY_PINS);
    setMessage('正在读取场景…');
    try {
      const result = await window.dlss.inspect(filePath);
      if (!result.ok || !result.scene) {
        setInspecting(false);
        setScene(null);
        const text = result.error || '读取场景失败';
        setInspectError(text);
        setMessage(text);
        if (fromProject) applyProject(fromProject, null);
        setProjectReady(true);
        return;
      }
      applyInspectedScene(result.scene, fromProject);
    } finally {
      window.setTimeout(() => {
        skipRemoteInspect.current = false;
      }, 500);
    }
  }

  async function openDcp(filePath: string) {
    const loaded = await window.dlss.loadDcp(filePath);
    if (!loaded.ok || !loaded.project) {
      setError(loaded.error || '无法打开工程');
      setMessage(loaded.error || '无法打开工程');
      return;
    }
    skipAutosave.current = true;
    setRoute('export');
    setDcpPath(filePath);
    setProjectReady(false);
    setJobs([]);
    activeJobIdRef.current = null;
    setStatus('idle');
    clearPreviewCanvas();
    applyProject(loaded.project, null);
    await window.dlss.setTitle(projectTitle(filePath));
    const listed = await window.dlss.rememberRecent(filePath);
    setRecents(listed);
    let blendPath = loaded.project.blend;
    if (!(await window.dlss.exists(blendPath))) {
      setInspectError(`找不到 blend：${blendPath}`);
      setMessage(`找不到 blend：${blendPath}`);
      const picked = await window.dlss.chooseBlend();
      if (!picked) {
        setProjectReady(true);
        skipAutosave.current = false;
        return;
      }
      blendPath = picked;
      setBlend(picked);
    }
    await inspectBlend(blendPath, {...loaded.project, blend: blendPath});
    skipAutosave.current = false;
  }

  async function pickOpenDcp() {
    const picked = await window.dlss.chooseDcpOpen();
    if (picked) await openDcp(picked);
  }

  async function newProject() {
    const picked = await window.dlss.chooseBlend();
    if (!picked) return;
    const suggested = defaultDcpPath(picked);
    const exists = await window.dlss.exists(suggested);
    const target = exists ? await window.dlss.chooseDcpSave(suggested) : suggested;
    if (!target) return;
    skipAutosave.current = true;
    setRoute('export');
    setDcpPath(target);
    setBlend(picked);
    setProjectReady(false);
    setJobs([]);
    activeJobIdRef.current = null;
    setStatus('idle');
    clearPreviewCanvas();
    setFilenameTemplate(DEFAULT_FILENAME_TEMPLATE);
    setOutputVersion(1);
    setHistory([]);
    // New projects start without NR buffer export — don't inherit the toggle
    // from whatever was open before (the inspect reset doesn't touch it).
    setNrBuffer(false);
    await window.dlss.setTitle(projectTitle(target));
    const stub = await window.dlss.saveDcp(target, emptyProject(picked));
    if (stub.ok) setRecents(await window.dlss.rememberRecent(target));
    await inspectBlend(picked);
    skipAutosave.current = false;
  }

  async function closeProject() {
    skipAutosave.current = true;
    setDcpPath(null);
    setProjectReady(false);
    setJobs([]);
    activeJobIdRef.current = null;
    setStatus('idle');
    setHistory([]);
    setScene(null);
    setInspectError(null);
    clearPreviewCanvas();
    await window.dlss.setTitle('DLSS Compositor');
    setRoute('project');
    skipAutosave.current = false;
  }

  useEffect(() => {
    let cancelled = false;
    void (async () => {
      const [launch, listed, isMax] = await Promise.all([
        window.dlss.launchFlags(),
        window.dlss.listRecent(),
        window.dlss.windowMaximized()
      ]);
      if (cancelled) return;
      setRecents(listed);
      setMaximized(isMax);
      if (launch.version) setAppVersion(launch.version);
      setElevated(Boolean(launch.elevated));
      let storedLog: string | null = null;
      try {
        storedLog = window.localStorage.getItem(CLI_LOG_KEY);
      } catch {
        /* ignore */
      }
      const effectiveLog =
        storedLog === 'on' ? true : storedLog === 'off' ? false : !launch.packaged;
      setCliLogState(effectiveLog);
      void window.dlss.setCliLog(effectiveLog);
      let stored: string | null = null;
      try {
        stored = window.localStorage.getItem(COLOR_SCHEME_KEY);
      } catch {
        /* ignore */
      }
      if (stored === 'dark' || stored === 'light' || stored === 'system') setColorSchemeState(stored);
      const page = launch.page;
      if (page === 'export' || page === 'jobs' || page === 'nrtool' || page === 'settings') {
        setRoute(page as Route);
      }
      if (launch.dcp) {
        await openDcp(launch.dcp);
      } else if (launch.blend) {
        setRoute('export');
        setBlend(launch.blend);
      }
    })();
    const runProbe = () => {
      void window.dlss.probe().then((result) => {
        if (!cancelled) setProbe(result);
      });
    };
    runProbe();
    const stopMax = window.dlss.onMaximized(setMaximized);
    return () => {
      cancelled = true;
      stopMax();
    };
    // openDcp is stable enough for cold start; listing launch flags once.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    if (status === 'running') return;
    setCurrentFrame((prev) => {
      const next = prev < frameStart || prev > frameEnd ? frameStart : prev;
      playheadRef.current = next;
      return next;
    });
  }, [frameStart, frameEnd, status]);

  useEffect(() => {
    if (hasScene && !canRR && mode === 'rr') setMode('sr');
  }, [hasScene, canRR, mode]);

  useEffect(() => {
    if (!nativeResEnabled && nativeResBuffers) setNativeResBuffers(false);
  }, [nativeResEnabled, nativeResBuffers]);

  // NGX RR 无超高质量/超高性能档：切到 RR 时直接改选，不给提示文案。
  useEffect(() => {
    if (mode === 'rr' && !rrQualityAllowed(quality)) setQuality('QUALITY');
  }, [mode, quality]);

  useEffect(() => {
    if (!fgEnabled && fg !== 0) setFg(0);
  }, [fgEnabled, fg]);

  useEffect(() => {
    void window.dlss.setTitle(
      route === 'project' ? 'DLSS Compositor' : `${projectTitle(dcpPath)} · ${ROUTE_LABELS[route]}`
    );
  }, [route, dcpPath]);

  useEffect(() => {
    if (route !== 'export' || !dcpPath || !projectReady || inspecting || skipAutosave.current) return;
    const snapshot = snapshotProject();
    const handle = window.setTimeout(() => {
      void window.dlss.saveDcp(dcpPath, snapshot);
    }, 450);
    return () => window.clearTimeout(handle);
  }, [
    route,
    dcpPath,
    projectReady,
    inspecting,
    blend,
    mode,
    pins,
    percentage,
    frameStart,
    frameEnd,
    fps,
    adaptive,
    noiseThreshold,
    samples,
    minSamples,
    timeLimit,
    useDenoising,
    denoiser,
    quality,
    outputWidth,
    outputHeight,
    outputFormat,
    exportDir,
    filenameTemplate,
    outputVersion,
    windowSize,
    useRamdisk,
    ramdiskGb,
    cachePath,
    nativeResBuffers,
    nrBuffer,
    perfPersistent,
    perfCameraCull,
    perfCameraCullMarginOverride,
    perfCameraCullMargin,
    perfDistanceCull,
    perfDistanceCullMarginOverride,
    perfDistanceCullMargin,
    perfTileSize,
    parallelSegments,
    fg,
    history
  ]);

  function patchJob(id: number | null, patch: Partial<QueueJob>) {
    if (id == null) return;
    if (activeJobRef.current?.id === id) {
      activeJobRef.current = {...activeJobRef.current, ...patch};
    }
    setJobs((prev) => prev.map((job) => (job.id === id ? {...job, ...patch} : job)));
  }

  function ensureActiveJob(patch: Partial<QueueJob> & {status: JobStatus}) {
    if (activeJobIdRef.current != null) {
      patchJob(activeJobIdRef.current, patch);
      return;
    }
    const id = jobSeq.current++;
    const card: QueueJob = {
      id,
      name: jobName,
      frameStart,
      frameEnd,
      outputWidth,
      outputHeight,
      mode,
      outputDir: exportDir,
      outputFormat,
      template: filenameTemplate,
      ...patch
    };
    activeJobIdRef.current = id;
    activeJobRef.current = card;
    setJobs((prev) => [...prev, card]);
  }

  function pushJobCard(): number {
    const id = jobSeq.current++;
    const card: QueueJob = {
      id,
      name: jobName,
      status: 'running',
      frameStart,
      frameEnd,
      outputWidth,
      outputHeight,
      mode,
      outputDir: exportDir,
      outputFormat,
      template: filenameTemplate
    };
    activeJobIdRef.current = id;
    activeJobRef.current = card;
    setJobs((prev) => [...prev, card]);
    return id;
  }

  function recordJobResult(jobId: number | null, recordStatus: JobRecord['status']) {
    const job = jobId != null ? activeJobRef.current : null;
    if (!job || job.id !== jobId) return;
    const record: JobRecord = {
      name: job.name,
      mode: job.mode,
      frameStart: job.frameStart,
      frameEnd: job.frameEnd,
      outputDir: job.outputDir,
      template: job.template,
      format: job.outputFormat,
      width: job.outputWidth,
      height: job.outputHeight,
      status: recordStatus,
      finishedAt: Date.now()
    };
    activeJobIdRef.current = null;
    activeJobRef.current = null;
    setHistory((existing) => [record, ...existing]);
  }

  async function startExport() {
    setError(null);
    if (mode === 'nr') {
      setError('DLSS5-NR 模型未到，接口已预留');
      return;
    }
    if (!blend) {
      setError('请选择 .blend 文件');
      return;
    }
    if (inspecting) {
      setError('正在读取场景，请稍候');
      return;
    }
    if (!hasScene) {
      setError(inspectError || '请先读取场景');
      return;
    }
    if (!canRR && mode === 'rr') {
      setError('当前引擎不是 Cycles，RR 不可用');
      return;
    }
    if (mode !== 'rr' && ngxWarning) {
      setError(userErrorMessage(ngxWarning));
      return;
    }
    const resolved = resolveFilenameTemplate(filenameTemplate, {
      job: jobTokenFromBlend(blend),
      mode,
      version: outputVersion,
      format: outputFormat
    });
    if (!resolved.ok) {
      setStatus('error');
      setError(resolved.error);
      setMessage(resolved.error);
      clearFrameTiming();
      return;
    }
    const jobId = pushJobCard();
    setRenderRatio(0);
    setPrepareRatio(0);
    setExportPhase('prepare');
    patchJob(jobId, {template: resolved.template});
    setStatus('running');
    clearPreviewCanvas();
    resetFrameTiming(frameStart);
    setMessage(mode === 'rr' ? 'RR 导出中' : 'SR 导出中');
    try {
      const result = await window.dlss.start({...request, outputTemplate: resolved.template});
      if (!result.ok) {
        const text = userErrorMessage(result.error ?? '导出失败');
        setStatus('error');
        setError(text);
        setMessage(text);
        clearFrameTiming();
        patchJob(jobId, {status: 'error'});
      }
    } catch (err) {
      const text = userErrorMessage(err instanceof Error ? err.message : '导出失败');
      setStatus('error');
      setError(text);
      setMessage(text);
      clearFrameTiming();
      patchJob(jobId, {status: 'error'});
    }
  }

  async function stopExport() {
    try {
      await window.dlss.stop();
    } catch (err) {
      const text = err instanceof Error ? err.message : '停止失败';
      setError(text);
    }
    setStatus('idle');
    setMessage('已停止');
    clearFrameTiming();
    patchJob(activeJobIdRef.current, {status: 'idle'});
  }

  function refreshProbe() {
    void window.dlss.probe().then((result) => setProbe(result));
  }

  useEffect(() => {
    return window.dlss.onEvent((event) => {
      if (event.type === 'open_dcp' && typeof event.path === 'string') {
        void openDcp(String(event.path));
      }
      if (event.type === 'blend_selected' && typeof event.blend === 'string') {
        setRoute('export');
        setBlend(event.blend);
        setInspecting(true);
        setInspectError(null);
        setMessage('正在读取场景…');
      }
      if (event.type === 'inspect_start') {
        setRoute('export');
        setInspecting(true);
        setInspectError(null);
        if (typeof event.blend === 'string') setBlend(event.blend);
        setMessage('正在读取场景…');
      }
      if (event.type === 'inspect_done') {
        if (skipRemoteInspect.current) return;
        applyInspectedScene(event as unknown as SceneState);
      }
      if (event.type === 'inspect_error') {
        if (skipRemoteInspect.current) return;
        setInspecting(false);
        setScene(null);
        const text = userErrorMessage(event.error ?? '读取场景失败');
        setInspectError(text);
        setMessage(text);
        setProjectReady(true);
      }
      if (event.type === 'ui_state') {
        if (typeof event.mode === 'string') setMode(String(event.mode));
        if (typeof event.nativeResBuffers === 'boolean') setNativeResBuffers(Boolean(event.nativeResBuffers));
        if (event.fg != null) setFg(parseFg(event.fg));
      }
      if (event.type === 'auto_export') {
        setRenderRatio(0);
        setRoute('export');
        if (typeof event.blend === 'string') setBlend(event.blend);
        if (typeof event.mode === 'string') setMode(event.mode);
        if (typeof event.quality === 'string') setQuality(event.quality);
        if (typeof event.frameStart === 'number') setFrameStart(event.frameStart);
        if (typeof event.frameEnd === 'number') setFrameEnd(event.frameEnd);
        if (typeof event.exportDir === 'string') setExportDir(event.exportDir);
        if (event.fg != null) setFg(parseFg(event.fg));
        setStatus('running');
        resetFrameTiming(typeof event.frameStart === 'number' ? event.frameStart : frameStart);
        setMessage('导出中');
        ensureActiveJob({
          name: typeof event.blend === 'string' ? basename(event.blend) : jobName,
          status: 'running',
          frameStart: typeof event.frameStart === 'number' ? event.frameStart : frameStart,
          frameEnd: typeof event.frameEnd === 'number' ? event.frameEnd : frameEnd,
          mode: typeof event.mode === 'string' ? event.mode : mode
        });
      }
      if (event.type === 'export_start' || event.type === 'job_started') {
        setRenderRatio(0);
        setStatus('running');
        setError(null);
        setPrepareRatio(0);
        if (event.fg != null) setFg(parseFg(event.fg));
        resetFrameTiming(frameStart);
        if (event.type === 'export_start') {
          // 帧导出前的 bake（写 dlssconfig/LUT）先行，占进度前 10%。
          setExportPhase('prepare');
          setMessage('准备导出');
        } else {
          setExportPhase('render');
          setMessage(event.mode === 'rr' ? 'RR 导出中' : 'SR 导出中');
        }
        ensureActiveJob({
          status: 'running',
          mode: typeof event.mode === 'string' ? event.mode : mode
        });
      }
      if (event.type === 'engine_progress' && typeof event.percent === 'number') {
        // orchestrator 归一后的进度：percent 是当前阶段的 0–1 比例。
        const phase = event.phase === 'prepare' || event.phase === 'finalize' ? event.phase : 'render';
        setExportPhase(phase);
        if (phase === 'prepare') {
          setPrepareRatio(Math.min(1, Math.max(0, event.percent)));
          setMessage('准备导出');
        } else if (phase === 'finalize') {
          setRenderRatio(1);
          setMessage('正在写视频');
        } else if (Number.isFinite(event.percent)) {
          setRenderRatio((prev) => Math.max(prev, Math.min(1, Math.max(0, event.percent as number))));
        }
      }
      if (event.type === 'preview_frame' && typeof event.jpeg_b64 === 'string' && event.jpeg_b64) {
        const canvas = previewCanvasRef.current;
        const dpr = window.devicePixelRatio || 1;
        const targetW = Math.min(2560, Math.max(320, Math.round((canvas?.clientWidth || 1280) * dpr)));
        const naturalW = typeof event.width === 'number' ? event.width : 0;
        pendingPreviewRef.current = {
          b64: event.jpeg_b64,
          resizeW: naturalW > targetW ? targetW : 0
        };
        pumpPreview();
      }
      if (event.type === 'job_phase' && typeof event.message === 'string' && event.message) {
        setMessage(String(event.message));
      }
      if ((event.type === 'progress' || event.type === 'frame_done') && typeof event.frame === 'number') {
        const mapped = mapBlenderFrame(event.frame, frameStart, frameEnd);
        if (mapped != null) {
          advancePlayhead(mapped);
          setMessage((prev) =>
            prev.startsWith('渲染') || prev.startsWith('引导') || prev.startsWith('重建')
              ? prev
              : `第 ${mapped} 帧`
          );
        }
      }
      if (event.type === 'export_done') {
        setStatus('done');
        setPlayhead(frameEnd);
        clearFrameTiming();
        setExportPhase(null);
        setPrepareRatio(0);
        setOutputVersion((prev) => nextOutputVersion(prev));
        setMessage(`已写入 ${exportDir}`);
        patchJob(activeJobIdRef.current, {status: 'done'});
        recordJobResult(activeJobIdRef.current, 'done');
      }
      if (event.type === 'export_error' || event.type === 'error') {
        const text = userErrorMessage(event.message ?? '导出失败');
        setStatus('error');
        setError(text);
        setMessage(text);
        setExportPhase(null);
        setPrepareRatio(0);
        clearFrameTiming();
        patchJob(activeJobIdRef.current, {status: 'error'});
        recordJobResult(activeJobIdRef.current, 'error');
      }
      if (event.type === 'stopped') {
        // 中止后回到刚加载项目的空闲态：进度条归零、文案就绪。
        setStatus((prev) => (prev === 'running' ? 'idle' : prev));
        setMessage('就绪');
        setPlayhead(frameStart);
        setExportPhase(null);
        setPrepareRatio(0);
        clearFrameTiming();
        if (typeof event.warning === 'string' && event.warning) {
          setError(userErrorMessage(event.warning));
        }
        patchJob(activeJobIdRef.current, {status: 'idle'});
      }
    });
  }, [frameStart, frameEnd, exportDir]);

  return {
    // navigation & chrome
    route,
    setRoute,
    navigateBack,
    colorScheme,
    setColorScheme,
    maximized,
    windowMinimize: () => void window.dlss.windowMinimize(),
    windowMaximize: () => void window.dlss.windowMaximize(),
    windowClose: () => void window.dlss.windowClose(),
    // project
    dcpPath,
    hasProject,
    recents,
    openDcp,
    pickOpenDcp,
    newProject,
    closeProject,
    // scene
    blend,
    setBlend,
    scene,
    hasScene,
    samplingLocked,
    canRR,
    inspecting,
    inspectError,
    inspectBlend,
    // settings
    pins,
    setPin,
    mode,
    setMode,
    quality,
    setQuality,
    percentage,
    setPercentage,
    frameStart,
    setFrameStart,
    frameEnd,
    setFrameEnd,
    fps,
    adaptive,
    setAdaptive,
    noiseThreshold,
    setNoiseThreshold,
    samples,
    setSamples,
    minSamples,
    setMinSamples,
    timeLimit,
    setTimeLimit,
    useDenoising,
    setUseDenoising,
    denoiser,
    setDenoiser,
    windowSize,
    setWindowSize,
    useRamdisk,
    setUseRamdisk,
    ramdiskGb,
    setRamdiskGb,
    cachePath,
    setCachePath,
    elevated,
    cliLog,
    setCliLog,
    nativeResBuffers,
    setNativeResBuffers,
    nativeResEnabled,
    nrBuffer,
    setNrBuffer,
    perfPersistent,
    setPerfPersistent,
    perfCameraCull,
    setPerfCameraCull,
    perfCameraCullMarginOverride,
    setPerfCameraCullMarginOverride,
    perfCameraCullMargin,
    setPerfCameraCullMargin,
    perfDistanceCull,
    setPerfDistanceCull,
    perfDistanceCullMarginOverride,
    setPerfDistanceCullMarginOverride,
    perfDistanceCullMargin,
    setPerfDistanceCullMargin,
    perfTileSize,
    setPerfTileSize,
    parallelSegments,
    setParallelSegments,
    fgEnabled,
    fgMultiplier,
    setFg,
    rrLocksDenoise,
    // export
    outputWidth,
    setOutputWidth,
    outputHeight,
    setOutputHeight,
    outputFormat,
    setOutputFormat,
    exportDir,
    setExportDir,
    filenameTemplate,
    setFilenameTemplate,
    outputVersion,
    // job state
    status,
    busy,
    message,
    error,
    setError,
    currentFrame,
    displayFrame,
    frameCount,
    outputFrames,
    playhead,
    progressPercent,
    exportPhase,
    prepareRatio,
    remainingFrames,
    canStart,
    etaText,
    frameTiming,
    registerPreviewCanvas,
    previewAvailable,
    startExport,
    stopExport,
    jobs,
    history,
    ngxWarning,
    // probe
    probe,
    refreshProbe
  };
}

export type AppState = ReturnType<typeof useAppState>;

const AppStateContext = createContext<AppState | null>(null);

let appVersion = '';

export function setAppVersion(version: string) {
  appVersion = version;
}

export function getAppVersion() {
  return appVersion || '0.4.10';
}

export function AppStateProvider({children}: {children: ReactNode}) {
  const state = useAppState();
  return <AppStateContext.Provider value={state}>{children}</AppStateContext.Provider>;
}

export function useApp(): AppState {
  const ctx = useContext(AppStateContext);
  if (!ctx) throw new Error('useApp must be used within AppStateProvider');
  return ctx;
}
