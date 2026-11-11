import {useCallback, useEffect, useRef, useState} from 'react';
import {ActionButton, Text} from '@react-spectrum/s2/ActionButton';
import {Workbench} from '../Workbench';
import {Disclosure, DisclosurePanel, DisclosureTitle} from '@react-spectrum/s2/Disclosure';
import {Picker, PickerItem} from '@react-spectrum/s2/Picker';
import {ProgressBar} from '@react-spectrum/s2/ProgressBar';
import {Slider} from '@react-spectrum/s2/Slider';
import {Switch} from '@react-spectrum/s2/Switch';
import {TextField} from '@react-spectrum/s2/TextField';
import {ToggleButton} from '@react-spectrum/s2/ToggleButton';
import {ToggleButtonGroup} from '@react-spectrum/s2/ToggleButtonGroup';
import FolderOpen from '@react-spectrum/s2/icons/FolderOpen';
import Pause from '@react-spectrum/s2/icons/Pause';
import Play from '@react-spectrum/s2/icons/Play';
import {useNrSession} from '../nrSession';
import {userErrorMessage} from '../errors';
import {style} from '@react-spectrum/s2/style' with {type: 'macro'};

const SETTINGS_KEY = 'dc-nrfg-settings';

type StoredSettings = {
  nrEnabled: boolean;
  model: number;
  style: number;
  intensity: number;
  localTone: number;
  localStructure: number;
  skinStructure: number;
  automask: boolean;
  uiCorrection: boolean;
  guidance: number;
  fgEnabled: boolean;
  fgMultiplier: number;
};

const DEFAULT_SETTINGS: StoredSettings = {
  nrEnabled: true,
  model: 0,
  style: 0,
  intensity: 1.0,
  localTone: 1.0,
  localStructure: 1.0,
  skinStructure: 0,
  automask: false,
  uiCorrection: false,
  guidance: 0,
  fgEnabled: true,
  fgMultiplier: 2,
};

const NR_STRENGTHS = [
  ['intensity', '强度'],
  ['localTone', '局部色调强度'],
  ['localStructure', '局部结构强度'],
  ['skinStructure', '皮肤结构强度']
] as const;

function loadSettings(): StoredSettings {
  try {
    const raw = window.localStorage.getItem(SETTINGS_KEY);
    if (!raw) return DEFAULT_SETTINGS;
    return {...DEFAULT_SETTINGS, ...(JSON.parse(raw) as Partial<StoredSettings>)};
  } catch {
    return DEFAULT_SETTINGS;
  }
}

/** Suggest the guidance folder for a picked color source:
 * `<dir>/<stem>_dlssbuffer`. A picked PNG is a sequence first frame, so its
 * trailing frame number is stripped (`name_0001.png` → `name_dlssbuffer`);
 * video stems are kept verbatim (`…_v3.mov` → `…_v3_dlssbuffer`). */
function suggestBufferDir(source: string): string {
  const norm = source.replace(/\\/g, '/');
  const dir = norm.includes('/') ? norm.slice(0, norm.lastIndexOf('/')) : '';
  if (!dir) return '';
  const file = norm.slice(norm.lastIndexOf('/') + 1);
  let stem = file.replace(/\.[^.]+$/, '');
  if (/\.png$/i.test(file)) stem = stem.replace(/\d+$/, '').replace(/[._\s]+$/, '');
  if (!stem) return '';
  return `${dir}/${stem}_dlssbuffer`;
}

function SettingsSection({
  title,
  defaultExpanded,
  insetControls,
  isDisabled,
  children
}: {
  title: string;
  defaultExpanded?: boolean;
  insetControls?: boolean;
  isDisabled?: boolean;
  children: React.ReactNode;
}) {
  return (
    <Disclosure
      defaultExpanded={defaultExpanded}
      isDisabled={isDisabled}
      size="S"
      density="compact"
      isQuiet
      styles={style({width: 'full'})}>
      <DisclosureTitle>{title}</DisclosureTitle>
      <DisclosurePanel>
        <div className={`settings-fields${insetControls ? ` ${style({paddingX: 16})}` : ''}`}>{children}</div>
      </DisclosurePanel>
    </Disclosure>
  );
}

export function NrToolPage({active}: {active: boolean}) {
  const nr = useNrSession();
  const {paths, setPaths, info, opening, exporting, exportPercent, setError, setMessage,
    openSession, closeSession, isExporting, beginExport, endExport, setExportTrigger} = nr;

  // ── viewport / timeline ────────────────────────────────────────────────────
  const [frame, setFrame] = useState(0);
  const [playing, setPlaying] = useState(false);
  const [evalMs, setEvalMs] = useState(0);

  // ── NR / FG settings ───────────────────────────────────────────────────────
  const [settings, setSettings] = useState<StoredSettings>(loadSettings);
  const [nrFormat, setNrFormat] = useState('prores_422hq');
  const [outputPath, setOutputPath] = useState('');
  const [outputDir, setOutputDir] = useState('');
  const [template, setTemplate] = useState('nr_####.png');

  // Latest-wins preview pump — mirrors the appState preview discipline: one
  // request in flight, only the newest requested frame survives a scrub.
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const bitmapRef = useRef<ImageBitmap | null>(null);
  const inFlightRef = useRef(false);
  const pendingRef = useRef<{frame: number; settings: Record<string, unknown>} | null>(null);
  const playingRef = useRef(false);
  const infoRef = useRef<NrToolInfo | null>(null);
  const frameRef = useRef(0);
  const settingsRef = useRef(settings as StoredSettings);
  const activeRef = useRef(active);
  infoRef.current = info;
  frameRef.current = frame;
  settingsRef.current = settings;
  activeRef.current = active;

  const settingsParams = useCallback(
    (s: StoredSettings) => ({
      nr_enabled: s.nrEnabled,
      nr_model: s.model,
      nr_style: s.style,
      nr_intensity: s.intensity,
      nr_local_tone: s.localTone,
      nr_local_structure: s.localStructure,
      nr_skin_structure: s.skinStructure,
      nr_automask: s.automask,
      nr_ui_correction: s.uiCorrection,
      nr_guidance: s.guidance,
      fg_enabled: s.fgEnabled,
      fg_multiplier: s.fgMultiplier
    }),
    []
  );

  // Preview cap: CLI downscales to this long edge before JPEG encode — matches
  // the display width so 4K sources don't ship full-res readback+JPEG over IPC.
  const previewTargetWidth = useCallback(() => {
    const dpr = window.devicePixelRatio || 1;
    const canvas = canvasRef.current;
    return Math.min(2560, Math.max(320, Math.round((canvas?.clientWidth || 1280) * dpr)));
  }, []);

  const drawJpeg = useCallback(async (b64: string, srcWidth: number | undefined, session: NrToolInfo) => {
    try {
      const canvas = canvasRef.current;
      if (!canvas) return;
      const binary = atob(b64);
      const bytes = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
      // CLI 已经按 preview_max_long_edge 出图；只有超宽的旧尺寸才需要重采样。
      const targetW = previewTargetWidth();
      const resize = typeof srcWidth === 'number' && srcWidth > targetW
        ? {resizeWidth: targetW, resizeQuality: 'medium' as const}
        : undefined;
      const bitmap = await createImageBitmap(new Blob([bytes], {type: 'image/jpeg'}), resize);
      const target = canvasRef.current;
      if (!target || !activeRef.current || isExporting() || infoRef.current !== session) {
        bitmap.close();
        return;
      }
      target.width = bitmap.width;
      target.height = bitmap.height;
      target.getContext('2d')?.drawImage(bitmap, 0, 0);
      bitmapRef.current?.close();
      bitmapRef.current = bitmap;
    } catch {
      /* malformed jpeg */
    }
  }, [isExporting, previewTargetWidth]);

  const pump = useCallback(async () => {
    if (inFlightRef.current) return;
    inFlightRef.current = true;
    try {
      for (;;) {
        const job = pendingRef.current;
        if (!job) break;
        pendingRef.current = null;
        // 导出期间 CLI 拒绝预览 —— 静默退出，别刷一屏错误。
        const session = infoRef.current;
        if (!activeRef.current || isExporting() || !session) break;
        const canDraw = () => activeRef.current && !isExporting() && infoRef.current === session;
        try {
          const r = await window.dlss.nrPreview(job.frame, {
            ...job.settings,
            preview_max_long_edge: previewTargetWidth()
          });
          if (!canDraw()) continue;
          if (r.ok && r.jpeg_b64) {
            // FG 只在播放时出插值帧：eval(N) 的插值位于 N-1 与 N 之间，
            // 先画插值再画实帧即得到 2x 顺序播放；seek/暂停只显示实帧。
            if (playingRef.current && r.interp_jpeg_b64) {
              await drawJpeg(r.interp_jpeg_b64, r.width, session);
            }
            await drawJpeg(r.jpeg_b64, r.width, session);
            setEvalMs(r.eval_ms);
          } else if (r.error) {
            setError(userErrorMessage(r.error));
            break;
          }
        } catch (err) {
          if (canDraw()) {
            setError(userErrorMessage(err instanceof Error ? err.message : '预览失败'));
          }
          break;
        }
        // Chained playback: advance only when the previous frame settled, so
        // the clip plays at the speed the engine actually manages.
        if (canDraw() && playingRef.current) {
          const frames = infoRef.current?.frames ?? 0;
          if (frames > 1) {
            const next = (job.frame + 1) % frames;
            setFrame(next);
            pendingRef.current = {frame: next, settings: settingsParams(settingsRef.current)};
          }
        }
      }
    } finally {
      inFlightRef.current = false;
      if (pendingRef.current) void pump();
    }
  }, [drawJpeg, isExporting, previewTargetWidth, settingsParams, setError]);

  const requestPreview = useCallback(
    (next: number) => {
      if (!activeRef.current || isExporting() || !infoRef.current) return;
      pendingRef.current = {frame: next, settings: settingsParams(settingsRef.current)};
      void pump();
    },
    [isExporting, pump, settingsParams]
  );

  // Keep the media, canvas and controls mounted while another page is visible.
  // Pause playback and discard queued preview work while hidden.
  useEffect(() => {
    if (active) {
      requestPreview(frameRef.current);
      return;
    }
    playingRef.current = false;
    setPlaying(false);
    pendingRef.current = null;
  }, [active, requestPreview]);
  useEffect(() => () => {
    playingRef.current = false;
    pendingRef.current = null;
    bitmapRef.current?.close();
  }, []);

  // Persist tuning so re-entering the tool restores the workspace (paths live
  // in the session store).
  useEffect(() => {
    try {
      window.localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
    } catch {
      /* private mode */
    }
  }, [settings]);

  // 选完色彩源（或改了 buffer 目录）→ 自动激活会话出预览。以「路径键」去重，
  // 手动改路径同样触发；正在打开/导出时跳过，等状态变化后重新评估。
  const openKey = `${paths.colorSource.trim()}|${paths.bufferDir.trim()}`;
  const openedKeyRef = useRef<string | null>(null);
  useEffect(() => {
    if (!active || opening || exporting) return;
    if (!paths.colorSource.trim()) {
      if (openedKeyRef.current !== null) {
        openedKeyRef.current = null;
        closeSession();
      }
      return;
    }
    if (openedKeyRef.current === openKey) return;
    const handle = window.setTimeout(() => {
      if (!activeRef.current || isExporting()) return;
      openedKeyRef.current = openKey;
      void openSession();
    }, 500);
    return () => window.clearTimeout(handle);
  }, [active, openKey, paths.colorSource, opening, exporting, isExporting, openSession, closeSession]);

  // A fresh session: rewind, suggest the output video, kick the first preview.
  useEffect(() => {
    if (!info) return;
    playingRef.current = false;
    setPlaying(false);
    setFrame(0);
    setEvalMs(0);
    if (!outputPath) setOutputPath(suggestVideoOut(paths.colorSource.trim()));
    requestPreview(0);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [info]);

  // Retune without scrubbing: re-evaluate the current frame (debounced while
  // the user drags a slider).
  const settingsKey = JSON.stringify(settings);
  const firstSettingsRun = useRef(true);
  useEffect(() => {
    if (firstSettingsRun.current) {
      firstSettingsRun.current = false;
      return;
    }
    if (!activeRef.current || !infoRef.current) return;
    const handle = window.setTimeout(() => {
      if (!playingRef.current) requestPreview(frameRef.current);
    }, 300);
    return () => window.clearTimeout(handle);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [settingsKey, requestPreview]);

  // 引导数据来自 dlssnrbuffer：没给目录就只有「无」可选，FG 无 MV 不可用。
  const hasBuffer = Boolean(paths.bufferDir.trim());
  useEffect(() => {
    if (!hasBuffer && settings.guidance !== 1) {
      setSettings((prev) => ({...prev, guidance: 1}));
    }
  }, [hasBuffer, settings.guidance]);
  useEffect(() => {
    if (!hasBuffer && settings.fgEnabled) {
      setSettings((prev) => ({...prev, fgEnabled: false}));
    }
  }, [hasBuffer, settings.fgEnabled]);

  function scrub(next: number) {
    playingRef.current = false;
    setPlaying(false);
    setFrame(next);
    requestPreview(next);
  }

  function togglePlay() {
    const session = infoRef.current;
    if (!session) return;
    const next = !playingRef.current;
    playingRef.current = next;
    setPlaying(next);
    if (next) {
      const start = frameRef.current + 1 >= session.frames ? 0 : frameRef.current;
      setFrame(start);
      requestPreview(start);
    }
  }

  const startExport = useCallback(() => {
    const session = infoRef.current;
    if (!activeRef.current || !session || openedKeyRef.current !== openKey || isExporting()) return;
    const params: Record<string, unknown> = {output_format: nrFormat, ...settingsParams(settings)};
    if (nrFormat === 'png_16') {
      if (!outputDir.trim()) {
        setError('请选择 PNG 输出目录');
        return;
      }
      params.output_dir = outputDir.trim();
      params.output_template = template.trim() || 'nr_####.png';
    } else {
      if (!outputPath.trim()) {
        setError('请选择输出视频路径');
        return;
      }
      params.output_path = outputPath.trim();
    }
    // 先锁定 UI（标题栏停止、进度条交换、时间轴禁用），RPC 失败再回滚 ——
    // 否则导出运行中 GUI 仍是空闲态，预览全部被 CLI 拒绝。
    if (!beginExport()) return;
    playingRef.current = false;
    pendingRef.current = null;
    setPlaying(false);
    window.dlss
      .nrExport(params)
      .then((started) => {
        if (!isExporting()) return;
        if (!started.ok) {
          endExport();
          setError(userErrorMessage(started.error ?? '导出失败'));
          return;
        }
        const stages = [settings.nrEnabled ? 'NR' : null, settings.fgEnabled ? 'FG' : null]
          .filter(Boolean)
          .join('/');
        setMessage(`导出中 · ${started.frames ?? session.frames} 帧 · ${stages || '直通'}`);
      })
      .catch((err: unknown) => {
        endExport();
        setError(userErrorMessage(err instanceof Error ? err.message : '导出失败'));
      });
  }, [beginExport, endExport, isExporting, nrFormat, openKey, outputDir, outputPath,
    setError, setMessage, settings, settingsParams, template]);

  // Only export once the visible paths match the media loaded by the CLI.
  const canExport = active && info != null && !opening && openedKeyRef.current === openKey;
  useEffect(() => {
    setExportTrigger(canExport ? startExport : null);
    return () => setExportTrigger(null);
  }, [canExport, setExportTrigger, startExport]);

  const aspect = info ? `${info.width} / ${info.height}` : '16 / 9';
  const fgConfigMissing = settings.fgEnabled && info != null && !info.camera_config;
  const outFrames =
    info && settings.fgEnabled
      ? info.frames + (info.frames - 1) * (settings.fgMultiplier - 1)
      : info?.frames ?? 0;

  return (
    <Workbench name="nrtool" controlsDisabled={exporting}>
      <aside className="export-settings">
        <SettingsSection title="媒体" defaultExpanded isDisabled={exporting}>
          <TextField
            label="色彩源（视频或 PNG 序列首帧）"
            size="S"
            value={paths.colorSource}
            onChange={(next) => setPaths((prev) => ({...prev, colorSource: next}))}
            isDisabled={opening || exporting}
          />
          <ActionButton
            size="S"
            isDisabled={opening || exporting}
            onPress={() => {
              void window.dlss.chooseNrVideo().then((picked) => {
                if (picked && activeRef.current && !isExporting()) {
                  // 单一入口：PNG 首帧 → 序列目录；视频 → 文件本身。
                  // buffer 子目录按 <名称>_dlssbuffer 自动建议。
                  const isPng = /\.png$/i.test(picked);
                  const colorSource = isPng
                    ? picked.replace(/\\/g, '/').slice(0, picked.replace(/\\/g, '/').lastIndexOf('/'))
                    : picked;
                  const bufferDir = suggestBufferDir(picked) || paths.bufferDir;
                  if (`${colorSource.trim()}|${bufferDir.trim()}` === openKey && openedKeyRef.current === openKey) {
                    void openSession();
                  } else {
                    setPaths(() => ({colorSource, bufferDir}));
                  }
                }
              });
            }}>
            <FolderOpen />
            <Text>选择色彩源</Text>
          </ActionButton>
          <div className="field-with-action">
            <TextField
              label="引导缓冲目录（MKV / 旧 EXR，可选）"
              size="S"
              value={paths.bufferDir}
              onChange={(next) => setPaths((prev) => ({...prev, bufferDir: next}))}
              isDisabled={opening || exporting}
            />
            <ActionButton
              size="S"
              isDisabled={opening || exporting}
              onPress={() => {
                void window.dlss.chooseDir('选择 dlssnrbuffer 目录').then((picked) => {
                  if (picked) setPaths((prev) => ({...prev, bufferDir: picked}));
                });
              }}>
              <FolderOpen />
              <Text>选择</Text>
            </ActionButton>
          </div>
          {info || opening ? <span className={style({font: 'detail-sm', color: 'neutral-subdued'})}>
            {info && info.buffer_width > 0 &&
            (info.buffer_width !== info.width || info.buffer_height !== info.height)
              ? `${nr.message} · 缓冲 ${info.buffer_width}×${info.buffer_height}`
              : nr.message}
          </span> : null}
        </SettingsSection>
        <SettingsSection title="NR" defaultExpanded insetControls isDisabled={exporting}>
          <Switch
            size="S"
            isSelected={settings.nrEnabled}
            onChange={(next) => setSettings((prev) => ({...prev, nrEnabled: next}))}
            isDisabled={exporting}>
            启用
          </Switch>
          <Picker
            label="风格"
            size="S"
            value={String(settings.style)}
            onChange={(value) => setSettings((prev) => ({...prev, style: Number(value)}))}
            isDisabled={exporting || !settings.nrEnabled}
            styles={style({width: 'full'})}>
            <PickerItem id="0">默认</PickerItem>
            <PickerItem id="1">自然</PickerItem>
            <PickerItem id="2">电影</PickerItem>
          </Picker>
          <Picker
            label="模型预设"
            size="S"
            value={String(settings.model)}
            onChange={(value) => setSettings((prev) => ({...prev, model: Number(value)}))}
            isDisabled={exporting || !settings.nrEnabled}
            styles={style({width: 'full'})}>
            <PickerItem id="0">自动（DLL 默认）</PickerItem>
            <PickerItem id="1">HT</PickerItem>
            <PickerItem id="2">CC Small FP8</PickerItem>
          </Picker>
          {NR_STRENGTHS.map(([key, label]) => (
            <Slider
              key={key}
              label={label}
              size="S"
              minValue={0}
              maxValue={2}
              step={0.05}
              value={settings[key]}
              onChange={(next) => setSettings((prev) => ({...prev, [key]: next}))}
              isDisabled={exporting || !settings.nrEnabled}
            />
          ))}
          <Switch
            size="S"
            isSelected={settings.automask}
            onChange={(next) => setSettings((prev) => ({...prev, automask: next}))}
            isDisabled={exporting || !settings.nrEnabled}>
            自动遮罩
          </Switch>
          <Switch
            size="S"
            isSelected={settings.uiCorrection}
            onChange={(next) => setSettings((prev) => ({...prev, uiCorrection: next}))}
            isDisabled={exporting || !settings.nrEnabled}>
            UI 修正
          </Switch>
          <Text styles={style({font: 'detail-sm'})}>引导模式</Text>
          <ToggleButtonGroup
            aria-label="引导模式"
            selectionMode="single"
            density="compact"
            isDisabled={exporting || !settings.nrEnabled}
            disallowEmptySelection
            selectedKeys={new Set([String(settings.guidance)])}
            onSelectionChange={(keys) => {
              const next = [...keys][0];
              if (next != null) setSettings((prev) => ({...prev, guidance: Number(next)}));
            }}
            styles={style({width: 'full'})}>
            <ToggleButton id="0" isDisabled={!hasBuffer}>
              <Text>全部</Text>
            </ToggleButton>
            <ToggleButton id="2" isDisabled={!hasBuffer}>
              <Text>运动</Text>
            </ToggleButton>
            <ToggleButton id="3" isDisabled={!hasBuffer}>
              <Text>深度</Text>
            </ToggleButton>
            <ToggleButton id="1">
              <Text>无</Text>
            </ToggleButton>
          </ToggleButtonGroup>
        </SettingsSection>
        <SettingsSection title="FG 帧生成" defaultExpanded isDisabled={exporting}>
          <Switch
            size="S"
            isSelected={settings.fgEnabled}
            onChange={(next) => setSettings((prev) => ({...prev, fgEnabled: next}))}
            isDisabled={exporting || !hasBuffer}>
            启用
          </Switch>
          <Picker
            label="倍数"
            size="S"
            value={String(settings.fgMultiplier)}
            onChange={(value) => setSettings((prev) => ({...prev, fgMultiplier: Number(value)}))}
            isDisabled={exporting || !settings.fgEnabled || !hasBuffer}
            styles={style({width: 'full'})}>
            <PickerItem id="2">2x（60fps）</PickerItem>
            <PickerItem id="3">3x</PickerItem>
            <PickerItem id="4">4x</PickerItem>
            <PickerItem id="5">5x</PickerItem>
            <PickerItem id="6">6x</PickerItem>
          </Picker>
          {fgConfigMissing ? (
            <span className={style({font: 'detail-sm', color: 'negative'})}>
              缺少 dlssconfig.json（缓冲目录内）— 无法帧生成
            </span>
          ) : null}
        </SettingsSection>
        <SettingsSection title="导出" defaultExpanded isDisabled={exporting}>
          <Picker
            label="编码"
            size="S"
            value={nrFormat}
            onChange={(value) => setNrFormat(String(value))}
            isDisabled={exporting}
            styles={style({width: 'full'})}>
            <PickerItem id="prores_422hq">ProRes 422 HQ</PickerItem>
            <PickerItem id="h265_hevc">H.265 (HEVC)</PickerItem>
            <PickerItem id="png_16">PNG 16-bit 序列</PickerItem>
          </Picker>
          {nrFormat === 'png_16' ? (
            <>
              <TextField
                label="文件名模板"
                size="S"
                value={template}
                onChange={setTemplate}
                isDisabled={exporting}
              />
              <div className="field-with-action">
                <TextField
                  label="目录"
                  size="S"
                  value={outputDir}
                  onChange={setOutputDir}
                  isDisabled={exporting}
                />
                <ActionButton
                  size="S"
                  isDisabled={exporting}
                  onPress={() => {
                    void window.dlss.chooseDir('选择 PNG 输出目录').then((picked) => {
                      if (picked) setOutputDir(picked);
                    });
                  }}>
                  <FolderOpen />
                  <Text>选择</Text>
                </ActionButton>
              </div>
            </>
          ) : (
            <div className="field-with-action">
              <TextField
                label="输出视频"
                size="S"
                value={outputPath}
                onChange={setOutputPath}
                isDisabled={exporting}
              />
              <ActionButton
                size="S"
                isDisabled={exporting}
                onPress={() => {
                  void window.dlss
                    .chooseNrVideoSave(outputPath || suggestVideoOut(paths.colorSource))
                    .then((picked) => {
                      if (picked) setOutputPath(picked);
                    });
                }}>
                <FolderOpen />
                <Text>选择</Text>
              </ActionButton>
            </div>
          )}
          {info ? (
            <span className={style({font: 'detail-sm', color: 'neutral-subdued'})}>
              输出 {outFrames} 帧
              {settings.fgEnabled ? ` · ${Math.round((info.fps || 30) * settings.fgMultiplier)}fps` : ''}
            </span>
          ) : null}
          {nr.error ? (
            <span className={style({font: 'detail-sm', color: 'negative'})}>{nr.error}</span>
          ) : null}
        </SettingsSection>
      </aside>

      <section className="preview-col">
        <div className={`preview-well ${style({backgroundColor: 'layer-1', borderRadius: 'lg'})}`}>
          <div className="preview-frame" style={{aspectRatio: aspect}}>
            <canvas ref={canvasRef} className="preview-image" aria-label="NR / FG 预览" />
            {!info && <div className={`preview-placeholder ${style({font: 'body', color: 'neutral-subdued'})}`}>
              {opening ? '打开中…' : '暂无媒体'}
            </div>}
          </div>
          <div className={`nr-bottom ${exporting ? 'is-busy' : 'is-idle'}`}>
            <div inert={!exporting} className={`nr-export-bar ${exporting ? '' : 'is-hidden'} ${style({backgroundColor: 'layer-2', borderRadius: 'lg'})}`}>
              <ProgressBar
                aria-label="导出进度"
                label={`导出中 ${exportPercent}%`}
                value={exportPercent}
                size="S"
                styles={style({width: 'full'})}
              />
            </div>
            <div inert={exporting} className={`nr-timeline ${exporting ? 'is-hidden' : ''} ${style({backgroundColor: 'layer-2', borderRadius: 'lg'})}`}>
              <ActionButton
                isQuiet
                aria-label={playing ? '暂停' : '播放'}
                isDisabled={!info || exporting}
                onPress={togglePlay}>
                <div slot="icon">{playing ? <Pause /> : <Play />}</div>
              </ActionButton>
              <Slider
                aria-label="帧"
                label="帧"
                size="S"
                minValue={1}
                maxValue={Math.max(1, info?.frames ?? 1)}
                step={1}
                value={frame + 1}
                onChange={next => scrub(next - 1)}
                isDisabled={!info || exporting}
                styles={style({flexGrow: 1, minWidth: 0})}
              />
              <span className={`nr-frame-label ${style({font: 'detail-sm'})}`}>
                {info
                  ? `共 ${info.frames} 帧${
                      evalMs > 0 ? ` · ${Math.round(evalMs)}ms` : ''
                    }`
                  : '— / —'}
              </span>
            </div>
          </div>
        </div>
      </section>
    </Workbench>
  );
}

/** Turn a color source path into a sibling `<stem>_nr.mov` suggestion. */
function suggestVideoOut(source: string): string {
  const norm = source.replace(/\\/g, '/').replace(/\/+$/, '');
  const isFile = /\.[a-z0-9]+$/i.test(norm);
  const dir = isFile ? norm.slice(0, norm.lastIndexOf('/')) : norm;
  const stem = isFile
    ? norm.slice(norm.lastIndexOf('/') + 1).replace(/\.[^.]+$/, '')
    : norm.split('/').filter(Boolean).pop() || 'nr';
  return `${dir}/${stem}_nr.mov`;
}
