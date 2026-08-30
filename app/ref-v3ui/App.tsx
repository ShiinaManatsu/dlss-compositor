import {useEffect, useMemo, useRef, useState, type ReactNode} from 'react';
import '@react-spectrum/s2/page.css';
import {
  ActionButton,
  Badge,
  Button,
  Checkbox,
  Disclosure,
  DisclosureContext,
  DisclosurePanel,
  DisclosureTitle,
  NumberField,
  Picker,
  PickerItem,
  ProgressBar,
  Provider,
  SegmentedControl,
  SegmentedControlItem,
  StatusLight,
  Switch,
  Text,
  TextField
} from '@react-spectrum/s2';
import FolderOpen from '@react-spectrum/s2/icons/FolderOpen';
import StopProcessing from '@react-spectrum/s2/icons/StopProcessing';
import {
  emptyFrameTiming,
  formatEtaLine,
  jobCount,
  mapBlenderFrame,
  mapEngineFrame,
  playheadRatio,
  recordFrameAdvance,
  startFrameTiming,
  type FrameTiming
} from './progress';
import {
  DEFAULT_FILENAME_TEMPLATE,
  DEFAULT_SPLITTERS,
  EMPTY_PINS,
  clampSplitters,
  cyclesDenoiseMeta,
  cyclesDeviceMeta,
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
  type DcpProject,
  type PinKey,
  type Pins,
  type RecentProject,
  type Splitters
} from './dcp';
import {userErrorMessage} from './errors';
import {HomeScreen} from './HomeScreen';
import {TitleBar} from './TitleBar';
import {style} from '@react-spectrum/s2/style' with {type: 'macro'};
import './shell.css';

type JobStatus = 'idle' | 'running' | 'done' | 'error';
type Screen = 'home' | 'editor';
type QueueJob = {
  id: number;
  name: string;
  status: JobStatus;
  frameStart: number;
  frameEnd: number;
  outputWidth: number;
  outputHeight: number;
  mode: string;
};

const QUALITY = [
  {id: 'DLAA', label: 'DLAA'},
  {id: 'ULTRA_QUALITY', label: '超高质量'},
  {id: 'QUALITY', label: '高质量'},
  {id: 'BALANCED', label: '平衡'},
  {id: 'PERFORMANCE', label: '性能'},
  {id: 'ULTRA_PERFORMANCE', label: '超高性能'}
];

const OUTPUTS = [
  {id: 'prores_422hq', label: 'ProRes 422 HQ'},
  {id: 'exr', label: 'OpenEXR（线性）'},
  {id: 'png_16', label: 'PNG 16-bit'}
];

const FG_OPTIONS = [
  {id: 'off', label: '关'},
  {id: '2x', label: '2x'},
  {id: '3x', label: '3x'},
  {id: '4x', label: '4x'},
  {id: '5x', label: '5x'},
  {id: '6x', label: '6x'}
];

const RECOMMENDED_PERCENT: Record<string, number> = {
  DLAA: 100,
  ULTRA_QUALITY: 77,
  QUALITY: 67,
  BALANCED: 58,
  PERFORMANCE: 50,
  ULTRA_PERFORMANCE: 33
};

function padFrame(n: number) {
  return String(n).padStart(4, '0');
}

function basename(filePath: string) {
  return filePath.replace(/\\/g, '/').split('/').filter(Boolean).pop() || filePath;
}

function ngxMinSize(outputW: number, outputH: number, quality: string) {
  const scale = quality === 'ULTRA_PERFORMANCE' ? 3 : 2;
  return [Math.max(1, Math.floor(outputW / scale)), Math.max(1, Math.floor(outputH / scale))] as const;
}

function percentError(outputW: number, outputH: number, percentage: number, quality: string) {
  const renderW = Math.max(1, Math.floor((outputW * percentage) / 100));
  const renderH = Math.max(1, Math.floor((outputH * percentage) / 100));
  const [minW, minH] = ngxMinSize(outputW, outputH, quality);
  if (renderW < minW || renderH < minH) {
    const rec = RECOMMENDED_PERCENT[quality] ?? 58;
    return `渲染 ${renderW}×${renderH} 低于 NGX ${quality} 下限 ${minW}×${minH}。请约 ${rec}%。`;
  }
  return null;
}

function statusLabel(status: JobStatus) {
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

function engineLabel(engine: string) {
  if (engine === 'CYCLES') return 'Cycles';
  if (engine.includes('EEVEE')) return 'EEVEE';
  return engine || '未知';
}

function Splitter({onDrag, kind}: {onDrag: (dx: number) => void; kind: 'settings' | 'jobs'}) {
  return (
    <div
      className={`splitter splitter-${kind}`}
      role="separator"
      aria-orientation="vertical"
      onPointerDown={(event) => {
        event.preventDefault();
        const handle = event.currentTarget;
        handle.dataset.active = 'true';
        let last = event.clientX;
        const move = (ev: PointerEvent) => {
          onDrag(ev.clientX - last);
          last = ev.clientX;
        };
        const up = () => {
          handle.dataset.active = 'false';
          window.removeEventListener('pointermove', move);
          window.removeEventListener('pointerup', up);
        };
        window.addEventListener('pointermove', move);
        window.addEventListener('pointerup', up);
      }}
    />
  );
}

function InspectorSection({
  id,
  title,
  defaultExpanded,
  children
}: {
  id: string;
  title: string;
  defaultExpanded?: boolean;
  children: ReactNode;
}) {
  return (
    <Disclosure id={id} defaultExpanded={defaultExpanded} size="S" density="compact" styles={style({width: 'full'})}>
      <DisclosureTitle>{title}</DisclosureTitle>
      <DisclosurePanel>
        <div className="inspector-fields">{children}</div>
      </DisclosurePanel>
    </Disclosure>
  );
}

function OverrideNumber({
  label,
  checked,
  onChecked,
  value,
  onChange,
  disabled,
  min,
  max,
  formatOptions
}: {
  label: string;
  checked: boolean;
  onChecked: (next: boolean) => void;
  value: number;
  onChange: (next: number) => void;
  disabled?: boolean;
  min?: number;
  max?: number;
  formatOptions?: Intl.NumberFormatOptions;
}) {
  return (
    <div className="override-row">
      <Checkbox size="S" isSelected={checked} onChange={onChecked} isDisabled={disabled}>
        {label}
      </Checkbox>
      <NumberField
        aria-label={label}
        size="S"
        value={value}
        onChange={onChange}
        isDisabled={!checked || disabled}
        hideStepper
        minValue={min}
        maxValue={max}
        formatOptions={formatOptions}
      />
    </div>
  );
}

export default function App() {
  const [screen, setScreen] = useState<Screen>('home');
  const [dcpPath, setDcpPath] = useState(null as string | null);
  const [recents, setRecents] = useState<RecentProject[]>([]);
  const [splitters, setSplitters] = useState<Splitters>(DEFAULT_SPLITTERS);
  const [filenameTemplate, setFilenameTemplate] = useState(DEFAULT_FILENAME_TEMPLATE);
  const [outputVersion, setOutputVersion] = useState(1);
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
  const [nativeResBuffers, setNativeResBuffers] = useState(false);
  const [fg, setFg] = useState(0);
  const [status, setStatus] = useState<JobStatus>('idle');
  const [currentFrame, setCurrentFrame] = useState(1);
  const [previewUrl, setPreviewUrl] = useState(null as string | null);
  const [previewPath, setPreviewPath] = useState(null as string | null);
  const [zoom, setZoom] = useState('fit');
  const [probe, setProbe] = useState(null as ProbeResult | null);
  const [message, setMessage] = useState('就绪');
  const [error, setError] = useState(null as string | null);
  const skipAutosave = useRef(false);
  const skipRemoteInspect = useRef(false);
  const [jobs, setJobs] = useState<QueueJob[]>([]);
  const jobSeq = useRef(1);
  const activeJobIdRef = useRef<number | null>(null);
  const playheadRef = useRef(1);
  const [frameTiming, setFrameTiming] = useState<FrameTiming>(emptyFrameTiming);

  const frameCount = jobCount(frameStart, frameEnd);
  const ngxWarning = percentError(outputWidth, outputHeight, percentage, quality);
  const busy = status === 'running';
  const jobName = blend ? basename(blend) : '未选择 blend';
  const hasScene = Boolean(scene?.ok);
  const samplingLocked = !hasScene || !scene?.cycles;
  const canRR = !hasScene || rrAllowed(scene?.engine);
  const nativeResEnabled = hasScene && nativeResAllowed(scene?.engine, mode);
  const fgEnabled = fgAllowed(mode);
  const fgMultiplier = fgEnabled ? parseFg(fg) : 0;
  const rrLocksDenoise = mode === 'rr';
  const outputFrames = outputFrameCount(frameCount, fgMultiplier);
  const displayFrame = Math.min(frameEnd, Math.max(frameStart, currentFrame));
  const playhead = playheadRatio(displayFrame, frameStart, frameEnd);
  const title = screen === 'editor' ? projectTitle(dcpPath) : 'DLSS Compositor';
  const remainingFrames = Math.max(0, frameEnd - displayFrame + 1);
  const etaText =
    busy && status === 'running' && !error ? formatEtaLine(frameTiming, remainingFrames) : null;

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
      overrides,
      nativeResBuffers: nativeResEnabled && nativeResBuffers,
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
    nativeResBuffers,
    nativeResEnabled,
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
      nativeResBuffers,
      fg: fgMultiplier,
      splitters,
      preview: previewPath
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
    setNativeResBuffers(Boolean(project.nativeResBuffers));
    setFg(fgAllowed(nextMode) ? parseFg(project.fg) : 0);
    setSplitters(clampSplitters(project.splitters));
    setPreviewPath(project.preview || null);
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
    setMessage(`已读取场景 · ${engineLabel(next.engine)}`);
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
    setScreen('editor');
    setDcpPath(filePath);
    setProjectReady(false);
    setJobs([]);
    activeJobIdRef.current = null;
    setStatus('idle');
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
    setScreen('editor');
    setDcpPath(target);
    setBlend(picked);
    setProjectReady(false);
    setJobs([]);
    activeJobIdRef.current = null;
    setStatus('idle');
    setFilenameTemplate(DEFAULT_FILENAME_TEMPLATE);
    setOutputVersion(1);
    setSplitters(DEFAULT_SPLITTERS);
    await window.dlss.setTitle(projectTitle(target));
    const stub = await window.dlss.saveDcp(target, emptyProject(picked));
    if (stub.ok) setRecents(await window.dlss.rememberRecent(target));
    await inspectBlend(picked);
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
      if (launch.dcp) {
        await openDcp(launch.dcp);
      } else if (launch.blend) {
        setScreen('editor');
        setBlend(launch.blend);
      }
    })();
    void window.dlss.probe().then((result) => {
      if (!cancelled) setProbe(result);
    });
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

  useEffect(() => {
    if (!fgEnabled && fg !== 0) setFg(0);
  }, [fgEnabled, fg]);

  useEffect(() => {
    void window.dlss.setTitle(title);
  }, [title]);

  useEffect(() => {
    if (screen !== 'editor' || !dcpPath || !projectReady || inspecting || skipAutosave.current) return;
    const snapshot = snapshotProject();
    const handle = window.setTimeout(() => {
      void window.dlss.saveDcp(dcpPath, snapshot);
    }, 450);
    return () => window.clearTimeout(handle);
  }, [
    screen,
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
    nativeResBuffers,
    fg,
    splitters,
    previewPath
  ]);

  useEffect(() => {
    return window.dlss.onEvent((event) => {
      if (event.type === 'blend_selected' && typeof event.blend === 'string') {
        setScreen('editor');
        setBlend(event.blend);
        setInspecting(true);
        setInspectError(null);
        setMessage('正在读取场景…');
      }
      if (event.type === 'inspect_start') {
        setScreen('editor');
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
        setScreen('editor');
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
        setStatus('running');
        setError(null);
        if (event.fg != null) setFg(parseFg(event.fg));
        resetFrameTiming(frameStart);
        setMessage(event.mode === 'rr' ? 'RR 导出中' : 'SR 导出中');
        ensureActiveJob({
          status: 'running',
          mode: typeof event.mode === 'string' ? event.mode : mode
        });
      }
      if (event.type === 'engine_progress') {
        const mapped = mapEngineFrame(
          Number(event.current_frame ?? event.dlss_current_frame ?? 0),
          frameStart,
          frameEnd
        );
        if (mapped != null) {
          advancePlayhead(mapped);
        }
      }
      if (event.type === 'preview_frame' && typeof event.jpeg_b64 === 'string' && event.jpeg_b64) {
        try {
          const binary = atob(event.jpeg_b64);
          const bytes = new Uint8Array(binary.length);
          for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
          const url = URL.createObjectURL(new Blob([bytes], {type: 'image/jpeg'}));
          setPreviewUrl((prev) => {
            if (prev) URL.revokeObjectURL(prev);
            return url;
          });
        } catch {
          /* ignore malformed jpeg */
        }
      }
      if (event.type === 'preview_frame' && typeof event.path === 'string' && event.path) {
        setPreviewPath(event.path);
        void window.dlss.previewDataUrl(event.path).then((url) => {
          if (url) setPreviewUrl(url);
        });
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
        setOutputVersion((prev) => nextOutputVersion(prev));
        setMessage(`已写入 ${exportDir}`);
        patchJob(activeJobIdRef.current, {status: 'done'});
      }
      if (event.type === 'export_error' || event.type === 'error') {
        const text = userErrorMessage(event.message ?? '导出失败');
        setStatus('error');
        setError(text);
        setMessage(text);
        clearFrameTiming();
        patchJob(activeJobIdRef.current, {status: 'error'});
      }
      if (event.type === 'stopped') {
        setStatus((prev) => (prev === 'running' ? 'idle' : prev));
        setMessage((prev) => (prev.endsWith('导出中') || prev.startsWith('第 ') ? '已停止' : prev));
        clearFrameTiming();
        if (typeof event.warning === 'string' && event.warning) {
          setError(userErrorMessage(event.warning));
        }
        setJobs((prev) =>
          prev.map((job) =>
            job.id === activeJobIdRef.current && job.status === 'running' ? {...job, status: 'idle'} : job
          )
        );
      }
    });
  }, [frameStart, frameEnd, exportDir]);

  async function openExportDir() {
    const picked = await window.dlss.chooseExportDir();
    if (picked) setExportDir(picked);
  }

  function patchJob(id: number | null, patch: Partial<QueueJob>) {
    if (id == null) return;
    setJobs((prev) => prev.map((job) => (job.id === id ? {...job, ...patch} : job)));
  }

  function ensureActiveJob(patch: Partial<QueueJob> & {status: JobStatus}) {
    if (activeJobIdRef.current != null) {
      patchJob(activeJobIdRef.current, patch);
      return;
    }
    const id = jobSeq.current++;
    activeJobIdRef.current = id;
    setJobs((prev) => [
      ...prev,
      {
        id,
        name: jobName,
        frameStart,
        frameEnd,
        outputWidth,
        outputHeight,
        mode,
        ...patch
      }
    ]);
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
      mode
    };
    setJobs((prev) => [...prev, card]);
    activeJobIdRef.current = id;
    return id;
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
    setStatus('running');
    setPreviewUrl((prev) => {
      if (prev) URL.revokeObjectURL(prev);
      return null;
    });
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

  const queueJobs: QueueJob[] =
    jobs.length > 0
      ? jobs
      : [
          {
            id: 0,
            name: jobName,
            status: 'idle',
            frameStart,
            frameEnd,
            outputWidth,
            outputHeight,
            mode
          }
        ];

  const ramOk = probe?.ramdisk.ok;
  const imdiskOk = probe?.imdisk.ok;
  const denoiseSummary = rrLocksDenoise
    ? 'RR · 关'
    : scene?.cycles
      ? scene.cycles.use_denoising
        ? `开 · ${scene.cycles.denoiser_label || scene.cycles.denoiser}`
        : '关'
      : '—';
  const sceneDeviceMeta = cyclesDeviceMeta(
    scene?.cycles?.device,
    scene?.cycles_compute?.compute_device_type
  );
  const sceneDenoiseMeta = cyclesDenoiseMeta(scene?.cycles);
  const ramStatus = ramOk
    ? `${probe?.ramdisk.letter}: DLSSRAM`
    : userErrorMessage(
        !imdiskOk
          ? probe?.imdisk.message || '请安装 ImDisk Toolkit'
          : probe?.ramdisk.message || 'R: 未挂载'
      );

  return (
    <Provider colorScheme="dark" locale="zh-CN">
      <div className={`shell ${screen === 'home' ? 'home' : 'editor'}`}>
        <TitleBar
          title={title}
          maximized={maximized}
          onMinimize={() => void window.dlss.windowMinimize()}
          onMaximize={() => void window.dlss.windowMaximize()}
          onClose={() => void window.dlss.windowClose()}
        />

        {screen === 'home' ? (
          <HomeScreen recents={recents} onOpen={(path) => void openDcp(path)} onPickOpen={() => void pickOpenDcp()} onNew={() => void newProject()} />
        ) : (
          <>
            <div className="workspace">
            <aside
              className={`queue ${style({
                display: 'flex',
                flexDirection: 'column',
                backgroundColor: 'layer-1'
              })}`}
              style={{width: splitters.queue}}>
              <div
                className={style({
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'space-between',
                  paddingX: 12,
                  paddingY: 4
                })}>
                <span className={style({font: 'detail-sm', color: 'gray-700'})}>任务</span>
                <span className={style({font: 'detail-sm', color: 'gray-600'})}>{queueJobs.length}</span>
              </div>
              <div className="queue-list">
                {queueJobs.map((job, index) => {
                  const selected = index === queueJobs.length - 1;
                  const jobBusy = job.status === 'running';
                  const jobPlayhead = jobBusy ? playhead : job.status === 'done' ? 1 : 0;
                  return (
                    <div key={job.id} className="queue-row" data-selected={selected ? 'true' : 'false'}>
                      <span className={`title ${style({font: 'ui-sm'})}`} title={job.name}>
                        {job.name}
                      </span>
                      <div className="queue-badge">
                        <Badge
                          size="S"
                          variant={
                            job.status === 'error'
                              ? 'negative'
                              : job.status === 'done'
                                ? 'positive'
                                : job.status === 'running'
                                  ? 'informative'
                                  : 'neutral'
                          }
                          fillStyle="subtle">
                          {statusLabel(job.status)}
                        </Badge>
                      </div>
                      {jobBusy ? (
                        <div className="queue-progress">
                          <ProgressBar
                            label={`${padFrame(job.frameStart)}–${padFrame(job.frameEnd)} · ${job.outputWidth}×${job.outputHeight} · ${job.mode.toUpperCase()}`}
                            value={jobPlayhead * 100}
                            size="S"
                            styles={style({width: 'full'})}
                          />
                        </div>
                      ) : (
                        <span className={`queue-range ${style({font: 'detail-sm', color: 'gray-600'})}`}>
                          {padFrame(job.frameStart)}–{padFrame(job.frameEnd)} · {job.outputWidth}×{job.outputHeight} · {job.mode.toUpperCase()}
                        </span>
                      )}
                    </div>
                  );
                })}
              </div>
            </aside>

            <Splitter
              kind="jobs"
              onDrag={(dx) => setSplitters((prev) => clampSplitters({...prev, queue: prev.queue - dx}))}
            />

            <div className="center">
            <section
              className={`monitor ${style({
                display: 'flex',
                flexDirection: 'column',
                backgroundColor: 'base'
              })}`}>
              <div
                className={style({
                  display: 'flex',
                  alignItems: 'center',
                  gap: 8,
                  paddingX: 8,
                  paddingY: 4,
                  backgroundColor: 'layer-1'
                })}>
                <span className={style({font: 'ui-sm', flexGrow: 1})}>预览</span>
                <span className={style({font: 'detail-sm', color: 'gray-600'})}>
                  {outputWidth}×{outputHeight} · {fps} fps
                </span>
                <SegmentedControl
                  aria-label="预览缩放"
                  selectedKey={zoom}
                  onSelectionChange={(key) => setZoom(String(key))}>
                  <SegmentedControlItem id="fit">适应</SegmentedControlItem>
                  <SegmentedControlItem id="1:1">1:1</SegmentedControlItem>
                </SegmentedControl>
              </div>
              <div className="monitor-well">
                <div className="monitor-frame" data-zoom={zoom}>
                  {previewUrl ? <img className="monitor-image" src={previewUrl} alt="当前预览帧" /> : null}
                  <div className={`monitor-caption ${style({font: 'detail-sm', color: 'white'})}`}>
                    {jobName} · {mode.toUpperCase()} · {OUTPUTS.find((item) => item.id === outputFormat)?.label}
                  </div>
                  <div className={`monitor-tc ${style({font: 'title-xs', color: 'white'})}`}>
                    {padFrame(displayFrame)} / {padFrame(frameEnd)}
                  </div>
                </div>
              </div>
            </section>

            <footer
              className={`timeline ${style({
                display: 'flex',
                alignItems: 'center',
                gap: 8,
                paddingX: 8,
                backgroundColor: 'layer-1'
              })}`}>
              {busy ? (
                <Button variant="secondary" size="S" onPress={() => void stopExport()}>
                  停止
                </Button>
              ) : (
                <Button variant="accent" size="S" isDisabled={!blend || inspecting} onPress={() => void startExport()}>
                  导出
                </Button>
              )}
              <ActionButton size="S" aria-label="停止" isDisabled={!busy} onPress={() => void stopExport()}>
                <StopProcessing />
              </ActionButton>
              <span className={`timeline-frame ${style({font: 'detail-sm', color: 'gray-700', width: 48})}`}>
                {padFrame(displayFrame)}
              </span>
              <div className="timeline-track" aria-hidden="true">
                <div className="timeline-fill" style={{width: `${playhead * 100}%`}} />
                <div className="timeline-playhead" style={{left: `${playhead * 100}%`}} />
              </div>
              <span className={`timeline-frame ${style({font: 'detail-sm', color: 'gray-700'})}`}>
                {padFrame(frameEnd)}
              </span>
            </footer>

            <div
              className={`status ${style({
                display: 'flex',
                alignItems: 'center',
                gap: 12,
                paddingX: 8,
                backgroundColor: 'layer-2'
              })}`}>
              <StatusLight size="S" variant={status === 'error' || inspectError ? 'negative' : busy || inspecting ? 'informative' : 'positive'}>
                {message}
              </StatusLight>
              <span
                className={
                  ramOk
                    ? style({font: 'detail-sm', color: 'gray-600'})
                    : style({font: 'detail-sm', color: 'red-700'})
                }>
                {ramStatus}
              </span>
              <span className={style({font: 'detail-sm', color: 'gray-600', marginStart: 'auto'})}>
                {etaText ? `${etaText} · ` : ''}
                {mode.toUpperCase()} · {QUALITY.find((item) => item.id === quality)?.label}
                {fgMultiplier >= 2 ? ` · ${fgMultiplier}x` : ''} · {outputFrames} 帧
              </span>
            </div>
            </div>

            <Splitter
              kind="settings"
              onDrag={(dx) => setSplitters((prev) => clampSplitters({...prev, inspector: prev.inspector + dx}))}
            />

            <aside
              className={`inspector ${style({
                display: 'flex',
                flexDirection: 'column',
                backgroundColor: 'layer-1'
              })}`}
              style={{width: splitters.inspector}}>
              <div className="inspector-body">
                <DisclosureContext.Provider value={{size: 'S', density: 'compact'}}>
                  <InspectorSection id="scene" title="场景" defaultExpanded>
                        <div className="field-with-action">
                          <TextField label="Blend" size="S" value={blend} onChange={setBlend} />
                          <ActionButton size="S" onPress={() => void pickOpenDcp()}>
                            <FolderOpen />
                            <Text>打开</Text>
                          </ActionButton>
                          <ActionButton
                            size="S"
                            isDisabled={!blend || inspecting || busy}
                            onPress={() => void inspectBlend(blend)}>
                            {inspecting ? '读取中…' : '读取'}
                          </ActionButton>
                        </div>
                        {inspectError ? (
                          <span className={style({font: 'detail-sm', color: 'red-700'})}>{inspectError}</span>
                        ) : hasScene ? (
                          <div className="scene-meta">
                            <span className={style({font: 'detail-sm', color: 'gray-600'})}>
                              {scene?.resolution_x}×{scene?.resolution_y} · {scene?.fps} fps · {engineLabel(scene?.engine ?? '')}
                              {sceneDeviceMeta ? ` · ${sceneDeviceMeta}` : ''}
                              {sceneDenoiseMeta ? ` · ${sceneDenoiseMeta}` : ''}
                            </span>
                            <span className={style({font: 'detail-sm', color: 'gray-600'})}>
                              时间轴 {scene?.frame_start}–{scene?.frame_end} · File Output {scene?.file_output_count}
                            </span>
                          </div>
                        ) : inspecting ? (
                          <span className={style({font: 'detail-sm', color: 'gray-600'})}>正在读取场景…</span>
                        ) : null}
                        <Picker
                          label="模式"
                          size="S"
                          value={mode}
                          onChange={(value) => setMode(String(value))}
                          styles={style({width: 'full'})}>
                          <PickerItem id="sr">SR 超分辨率</PickerItem>
                          <PickerItem id="rr" isDisabled={!canRR}>
                            RR 光线重建
                          </PickerItem>
                        </Picker>
                  </InspectorSection>

                  <InspectorSection id="overrides" title="覆盖">
                        {mode === 'rr' ? (
                          <span className={style({font: 'detail-sm', color: 'gray-600'})}>
                            RR 仅支持质量档：{quality} → {RECOMMENDED_PERCENT[quality] ?? 58}%
                          </span>
                        ) : (
                          <>
                        <OverrideNumber
                          label="渲染百分比"
                          checked={pins.percentage}
                          onChecked={(next) => setPin('percentage', next)}
                          value={percentage}
                          onChange={setPercentage}
                          disabled={!hasScene || busy}
                          min={1}
                          max={100}
                        />
                        {ngxWarning && pins.percentage ? (
                          <span className={style({font: 'detail-sm', color: 'red-700'})}>{ngxWarning}</span>
                        ) : null}
                          </>
                        )}
                        <OverrideNumber
                          label="起始帧"
                          checked={pins.frameStart}
                          onChecked={(next) => setPin('frameStart', next)}
                          value={frameStart}
                          onChange={setFrameStart}
                          disabled={!hasScene || busy}
                        />
                        <OverrideNumber
                          label="结束帧"
                          checked={pins.frameEnd}
                          onChecked={(next) => setPin('frameEnd', next)}
                          value={frameEnd}
                          onChange={setFrameEnd}
                          disabled={!hasScene || busy}
                        />
                        <div className="override-row">
                          <Checkbox
                            size="S"
                            isSelected={pins.adaptive}
                            onChange={(next) => setPin('adaptive', next)}
                            isDisabled={samplingLocked || busy}>
                            Adaptive 采样
                          </Checkbox>
                          {pins.adaptive ? (
                            <Switch size="S" isSelected={adaptive} onChange={setAdaptive} isDisabled={busy}>
                              {adaptive ? '开' : '关'}
                            </Switch>
                          ) : (
                            <span className={`override-readonly ${style({font: 'detail-sm', color: 'gray-600'})}`}>
                              {samplingLocked ? '—' : adaptive ? '开' : '关'}
                            </span>
                          )}
                        </div>
                        <OverrideNumber
                          label="Noise Threshold"
                          checked={pins.noiseThreshold}
                          onChecked={(next) => setPin('noiseThreshold', next)}
                          value={noiseThreshold}
                          onChange={setNoiseThreshold}
                          disabled={samplingLocked || busy}
                          min={0}
                          formatOptions={{minimumFractionDigits: 2, maximumFractionDigits: 4}}
                        />
                        <OverrideNumber
                          label="Max Samples"
                          checked={pins.samples}
                          onChecked={(next) => setPin('samples', next)}
                          value={samples}
                          onChange={setSamples}
                          disabled={samplingLocked || busy}
                          min={1}
                        />
                        <OverrideNumber
                          label="Min Samples"
                          checked={pins.minSamples}
                          onChecked={(next) => setPin('minSamples', next)}
                          value={minSamples}
                          onChange={setMinSamples}
                          disabled={samplingLocked || busy}
                          min={0}
                        />
                        <OverrideNumber
                          label="Time Limit"
                          checked={pins.timeLimit}
                          onChecked={(next) => setPin('timeLimit', next)}
                          value={timeLimit}
                          onChange={setTimeLimit}
                          disabled={samplingLocked || busy}
                          min={0}
                        />
                        {rrLocksDenoise ? (
                          <div className="override-row">
                            <span className={style({font: 'ui-sm'})}>Denoise</span>
                            <span className={`override-readonly ${style({font: 'detail-sm', color: 'gray-600'})}`}>RR · 关</span>
                          </div>
                        ) : (
                          <>
                            <div className="override-row">
                              <Checkbox
                                size="S"
                                isSelected={pins.denoise}
                                onChange={(next) => setPin('denoise', next)}
                                isDisabled={samplingLocked || busy}>
                                Denoise
                              </Checkbox>
                              {pins.denoise ? (
                                <Switch size="S" isSelected={useDenoising} onChange={setUseDenoising} isDisabled={busy}>
                                  {useDenoising ? '开' : '关'}
                                </Switch>
                              ) : (
                                <span className={`override-readonly ${style({font: 'detail-sm', color: 'gray-600'})}`}>
                                  {denoiseSummary}
                                </span>
                              )}
                            </div>
                            {pins.denoise && useDenoising ? (
                              <Picker
                                label="降噪器"
                                size="S"
                                value={denoiser}
                                onChange={(value) => setDenoiser(String(value))}
                                styles={style({width: 'full'})}>
                                <PickerItem id="OPENIMAGEDENOISE">OIDN</PickerItem>
                                <PickerItem id="OPTIX">OptiX</PickerItem>
                              </Picker>
                            ) : null}
                          </>
                        )}
                  </InspectorSection>

                  <InspectorSection id="export" title="导出" defaultExpanded>
                        <Picker
                          label="质量"
                          size="S"
                          value={quality}
                          onChange={(value) => setQuality(String(value))}
                          styles={style({width: 'full'})}>
                          {QUALITY.map((item) => (
                            <PickerItem key={item.id} id={item.id}>
                              {item.label}
                            </PickerItem>
                          ))}
                        </Picker>
                        <span className={style({font: 'detail-sm', color: 'gray-600'})}>
                          推荐 {RECOMMENDED_PERCENT[quality] ?? 58}%
                        </span>
                        <div className="row-2">
                          <NumberField label="宽" size="S" value={outputWidth} onChange={setOutputWidth} hideStepper />
                          <NumberField label="高" size="S" value={outputHeight} onChange={setOutputHeight} hideStepper />
                        </div>
                        {nativeResEnabled ? (
                          <Checkbox
                            size="S"
                            isSelected={nativeResBuffers}
                            onChange={setNativeResBuffers}
                            isDisabled={busy}>
                            使用原生分辨率缓冲
                          </Checkbox>
                        ) : null}
                        <Picker
                          label="帧生成"
                          size="S"
                          value={fgEnabled && fgMultiplier >= 2 ? `${fgMultiplier}x` : 'off'}
                          onChange={(value) => setFg(parseFg(value))}
                          isDisabled={!fgEnabled || busy}
                          styles={style({width: 'full'})}>
                          {FG_OPTIONS.map((item) => (
                            <PickerItem key={item.id} id={item.id}>
                              {item.label}
                            </PickerItem>
                          ))}
                        </Picker>
                        <div className="scene-meta">
                          <span className={style({font: 'detail-sm', color: 'gray-600'})}>
                            色彩 {hasScene ? scene?.view_transform || '—' : '—'}
                          </span>
                        </div>
                        <Picker
                          label="编码"
                          size="S"
                          value={outputFormat}
                          onChange={(value) => setOutputFormat(String(value))}
                          styles={style({width: 'full'})}>
                          {OUTPUTS.map((item) => (
                            <PickerItem key={item.id} id={item.id}>
                              {item.label}
                            </PickerItem>
                          ))}
                        </Picker>
                        <TextField
                          label="文件名模板"
                          size="S"
                          value={filenameTemplate}
                          onChange={setFilenameTemplate}
                        />
                        <span className={style({font: 'detail-sm', color: 'gray-600'})}>
                          v{outputVersion} · {'{job} {version} {frame} {mode}'}
                        </span>
                        <div className="field-with-action">
                          <TextField label="目录" size="S" value={exportDir} onChange={setExportDir} />
                          <ActionButton size="S" onPress={() => void openExportDir()}>
                            选择
                          </ActionButton>
                        </div>
                        {error ? (
                          <span className={style({font: 'detail-sm', color: 'red-700'})}>{error}</span>
                        ) : null}
                  </InspectorSection>

                  <InspectorSection id="advanced" title="高级">
                        <NumberField
                          label="消费窗口"
                          size="S"
                          minValue={4}
                          maxValue={8}
                          value={windowSize}
                          onChange={setWindowSize}
                        />
                        <span
                          className={
                            imdiskOk
                              ? style({font: 'detail-sm', color: 'gray-600'})
                              : style({font: 'detail-sm', color: 'red-700'})
                          }>
                          {imdiskOk
                            ? `ImDisk ${probe?.imdisk.path ?? '已安装'}`
                            : userErrorMessage(probe?.imdisk.message ?? '请安装 ImDisk Toolkit')}
                        </span>
                        <span
                          className={
                            ramOk
                              ? style({font: 'detail-sm', color: 'gray-600'})
                              : style({font: 'detail-sm', color: 'red-700'})
                          }>
                          {ramOk
                            ? `${probe?.ramdisk.letter}: ${probe?.ramdisk.label || 'DLSSRAM'}`
                            : userErrorMessage(probe?.ramdisk.message ?? '未找到 DLSSRAM')}
                        </span>
                  </InspectorSection>
                </DisclosureContext.Provider>
              </div>
            </aside>
            </div>
          </>
        )}
      </div>
    </Provider>
  );
}
