import {useCallback, useEffect, useRef, useState} from 'react';
import {
  ActionButton,
  Button,
  Divider,
  Disclosure,
  DisclosurePanel,
  DisclosureTitle,
  Picker,
  PickerItem,
  ProgressBar,
  Slider,
  Switch,
  Text,
  TextField,
  ToggleButton,
  ToggleButtonGroup
} from '@react-spectrum/s2';
import FolderOpen from '@react-spectrum/s2/icons/FolderOpen';
import Pause from '@react-spectrum/s2/icons/Pause';
import Play from '@react-spectrum/s2/icons/Play';
import StopProcessing from '@react-spectrum/s2/icons/StopProcessing';
import {useNrSession} from '../nrSession';
import {userErrorMessage} from '../errors';
import {style} from '@react-spectrum/s2/style' with {type: 'macro'};

const SETTINGS_KEY = 'dc-nr-settings';

type StoredSettings = {
  style: number;
  intensity: number;
  localTone: number;
  localStructure: number;
  automask: boolean;
  guidance: number;
};

const DEFAULT_SETTINGS: StoredSettings = {
  style: 2,
  intensity: 1.0,
  localTone: 1.0,
  localStructure: 0.45,
  automask: false,
  guidance: 0,
};

function loadSettings(): StoredSettings {
  try {
    const raw = window.localStorage.getItem(SETTINGS_KEY);
    if (!raw) return DEFAULT_SETTINGS;
    return {...DEFAULT_SETTINGS, ...(JSON.parse(raw) as Partial<StoredSettings>)};
  } catch {
    return DEFAULT_SETTINGS;
  }
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

/** Suggest the dlssnrbuffer folder for a picked video: exports now write the
 * guidance sequence into `<video dir>/<video name>/` (no extension). */
function suggestBufferDir(source: string): string {
  const norm = source.replace(/\\/g, '/');
  if (!/\.[a-z0-9]+$/i.test(norm)) return '';
  const dir = norm.slice(0, norm.lastIndexOf('/'));
  const stem = norm.slice(norm.lastIndexOf('/') + 1).replace(/\.[^.]+$/, '');
  return `${dir}/${stem}`;
}

function SettingsSection({
  title,
  defaultExpanded,
  children
}: {
  title: string;
  defaultExpanded?: boolean;
  children: React.ReactNode;
}) {
  return (
    <Disclosure
      defaultExpanded={defaultExpanded}
      size="S"
      density="compact"
      isQuiet
      styles={style({width: 'full'})}>
      <DisclosureTitle>{title}</DisclosureTitle>
      <DisclosurePanel>
        <div className="settings-fields">{children}</div>
      </DisclosurePanel>
    </Disclosure>
  );
}

export function NrToolPage() {
  const nr = useNrSession();
  const {paths, info, opening, exporting, exportPercent, setError} = nr;

  // ── viewport / timeline ────────────────────────────────────────────────────
  const [frame, setFrame] = useState(0);
  const [playing, setPlaying] = useState(false);
  const [evalMs, setEvalMs] = useState(0);

  // ── NR settings ────────────────────────────────────────────────────────────
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
  infoRef.current = info;
  frameRef.current = frame;
  settingsRef.current = settings;

  const settingsParams = useCallback(
    (s: StoredSettings) => ({
      nr_style: s.style,
      nr_intensity: s.intensity,
      nr_local_tone: s.localTone,
      nr_local_structure: s.localStructure,
      nr_automask: s.automask,
      nr_guidance: s.guidance
    }),
    []
  );

  const drawJpeg = useCallback(async (b64: string) => {
    try {
      const canvas = canvasRef.current;
      if (!canvas) return;
      const binary = atob(b64);
      const bytes = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
      const dpr = window.devicePixelRatio || 1;
      const targetW = Math.min(1920, Math.max(320, Math.round((canvas.clientWidth || 1280) * dpr)));
      const bitmap = await createImageBitmap(new Blob([bytes], {type: 'image/jpeg'}), {
        resizeWidth: targetW,
        resizeQuality: 'medium'
      });
      const target = canvasRef.current;
      if (!target) {
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
  }, []);

  const pump = useCallback(async () => {
    if (inFlightRef.current) return;
    inFlightRef.current = true;
    try {
      for (;;) {
        const job = pendingRef.current;
        if (!job) break;
        pendingRef.current = null;
        try {
          const r = await window.dlss.nrPreview(job.frame, job.settings);
          if (r.ok && r.jpeg_b64) {
            await drawJpeg(r.jpeg_b64);
            setEvalMs(r.eval_ms);
          } else if (r.error) {
            setError(userErrorMessage(r.error));
            break;
          }
        } catch (err) {
          setError(userErrorMessage(err instanceof Error ? err.message : '预览失败'));
          break;
        }
        // Chained playback: advance only when the previous frame settled, so
        // the clip plays at the speed the engine actually manages.
        if (playingRef.current) {
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
  }, [drawJpeg, settingsParams, setError]);

  const requestPreview = useCallback(
    (next: number) => {
      pendingRef.current = {frame: next, settings: settingsParams(settingsRef.current)};
      void pump();
    },
    [pump, settingsParams]
  );

  // Leaving the page releases the 4K NR session (feature + textures) in the CLI.
  // closeSession is useCallback([])-stable in the store, so this is unmount-only.
  const {closeSession} = nr;
  useEffect(
    () => () => {
      playingRef.current = false;
      pendingRef.current = null;
      void closeSession();
    },
    [closeSession]
  );

  // Persist tuning so re-entering the tool restores the workspace (paths live
  // in the session store).
  useEffect(() => {
    try {
      window.localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
    } catch {
      /* private mode */
    }
  }, [settings]);

  // A fresh session (打开/重新打开): rewind, suggest the output video, kick the
  // first preview.
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
    if (!infoRef.current) return;
    const handle = window.setTimeout(() => {
      if (!playingRef.current) requestPreview(frameRef.current);
    }, 300);
    return () => window.clearTimeout(handle);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [settingsKey, requestPreview]);

  // 引导数据来自 dlssnrbuffer：没给目录就只有「无」可选。
  const hasBuffer = Boolean(paths.bufferDir.trim());
  useEffect(() => {
    if (!hasBuffer && settings.guidance !== 1) {
      setSettings((prev) => ({...prev, guidance: 1}));
    }
  }, [hasBuffer, settings.guidance]);

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

  async function startExport() {
    const session = infoRef.current;
    if (!session) return;
    nr.setError(null);
    const params: Record<string, unknown> = {output_format: nrFormat, ...settingsParams(settings)};
    if (nrFormat === 'png_16') {
      if (!outputDir.trim()) {
        nr.setError('请选择 PNG 输出目录');
        return;
      }
      params.output_dir = outputDir.trim();
      params.output_template = template.trim() || 'nr_####.png';
    } else {
      if (!outputPath.trim()) {
        nr.setError('请选择输出视频路径');
        return;
      }
      params.output_path = outputPath.trim();
    }
    try {
      const started = await window.dlss.nrExport(params);
      if (!started.ok) {
        nr.setError(userErrorMessage(started.error ?? '导出失败'));
        return;
      }
      playingRef.current = false;
      setPlaying(false);
      nr.setMessage(`NR 导出中 · ${session.frames} 帧 · ${settingsRef.current.intensity.toFixed(2)}`);
    } catch (err) {
      nr.setError(userErrorMessage(err instanceof Error ? err.message : '导出失败'));
    }
  }

  async function stopExport() {
    try {
      await window.dlss.nrStop();
    } catch (err) {
      nr.setError(userErrorMessage(err instanceof Error ? err.message : '停止失败'));
    }
  }

  const aspect = info ? `${info.width} / ${info.height}` : '16 / 9';
  const exportLabel = exporting
    ? `NR 导出中 ${exportPercent}%`
    : exportPercent >= 100
      ? 'NR 导出完成'
      : null;

  return (
    <div className="export-body">
      <aside className="export-settings">
        <SettingsSection title="媒体" defaultExpanded>
          <TextField
            label="色彩源（视频或 PNG 序列目录）"
            size="S"
            value={paths.colorSource}
            onChange={(next) => nr.setPaths((prev) => ({...prev, colorSource: next}))}
            isDisabled={opening || exporting}
          />
          <div className="row-2">
            <ActionButton
              size="S"
              isDisabled={opening || exporting}
              onPress={() => {
                void window.dlss.chooseNrVideo().then((picked) => {
                  if (picked) {
                    // The buffer subfolder is named after the video — suggest it
                    // alongside the color source.
                    nr.setPaths((prev) => ({
                      ...prev,
                      colorSource: picked,
                      bufferDir: suggestBufferDir(picked) || prev.bufferDir
                    }));
                  }
                });
              }}>
              <FolderOpen />
              <Text>选视频</Text>
            </ActionButton>
            <ActionButton
              size="S"
              isDisabled={opening || exporting}
              onPress={() => {
                void window.dlss.chooseDir('选择 PNG 序列目录').then((picked) => {
                  if (picked) nr.setPaths((prev) => ({...prev, colorSource: picked}));
                });
              }}>
              <FolderOpen />
              <Text>选序列目录</Text>
            </ActionButton>
          </div>
          <div className="field-with-action">
            <TextField
              label="dlssnrbuffer 目录"
              size="S"
              value={paths.bufferDir}
              onChange={(next) => nr.setPaths((prev) => ({...prev, bufferDir: next}))}
              isDisabled={opening || exporting}
            />
            <ActionButton
              size="S"
              isDisabled={opening || exporting}
              onPress={() => {
                void window.dlss.chooseDir('选择 dlssnrbuffer 目录').then((picked) => {
                  if (picked) nr.setPaths((prev) => ({...prev, bufferDir: picked}));
                });
              }}>
              <FolderOpen />
              <Text>选择</Text>
            </ActionButton>
          </div>
          {info ? (
            <span className={style({font: 'detail-sm', color: 'gray-600'})}>
              {nr.message}
              {info.buffer_width !== info.width || info.buffer_height !== info.height
                ? ` · 缓冲 ${info.buffer_width}×${info.buffer_height}`
                : ''}
            </span>
          ) : (
            <span className={style({font: 'detail-sm', color: 'gray-600'})}>{nr.message}</span>
          )}
        </SettingsSection>
        <Divider size="S" />
        <SettingsSection title="NR 设置" defaultExpanded>
          <Picker
            label="降噪风格"
            size="S"
            value={String(settings.style)}
            onChange={(value) => setSettings((prev) => ({...prev, style: Number(value)}))}
            isDisabled={exporting}
            styles={style({width: 'full'})}>
            <PickerItem id="0">默认</PickerItem>
            <PickerItem id="1">自然</PickerItem>
            <PickerItem id="2">电影</PickerItem>
          </Picker>
          <Slider
            label="降噪强度"
            size="S"
            minValue={0}
            maxValue={1}
            step={0.05}
            value={settings.intensity}
            onChange={(next) => setSettings((prev) => ({...prev, intensity: next}))}
            isDisabled={exporting}
          />
          <Slider
            label="局部色调"
            size="S"
            minValue={0}
            maxValue={1}
            step={0.05}
            value={settings.localTone}
            onChange={(next) => setSettings((prev) => ({...prev, localTone: next}))}
            isDisabled={exporting}
          />
          <Slider
            label="局部结构"
            size="S"
            minValue={0}
            maxValue={1}
            step={0.05}
            value={settings.localStructure}
            onChange={(next) => setSettings((prev) => ({...prev, localStructure: next}))}
            isDisabled={exporting}
          />
          <Switch
            size="S"
            isSelected={settings.automask}
            onChange={(next) => setSettings((prev) => ({...prev, automask: next}))}
            isDisabled={exporting}>
            自动遮罩
          </Switch>
          <Text styles={style({font: 'detail-sm'})}>引导模式</Text>
          <ToggleButtonGroup
            aria-label="引导模式"
            selectionMode="single"
            density="compact"
            isDisabled={exporting}
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
        <Divider size="S" />
        <SettingsSection title="导出" defaultExpanded>
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
          {exporting ? (
            <Button variant="secondary" size="S" onPress={() => void stopExport()}>
              <StopProcessing />
              <Text>停止</Text>
            </Button>
          ) : (
            <Button
              variant="accent"
              size="S"
              isDisabled={!info}
              onPress={() => void startExport()}>
              <Text>导出</Text>
            </Button>
          )}
          {nr.error ? (
            <span className={style({font: 'detail-sm', color: 'red-700'})}>{nr.error}</span>
          ) : null}
        </SettingsSection>
      </aside>

      <section className="preview-col">
        <div className="preview-well">
          <div className="preview-frame" style={{aspectRatio: aspect}}>
            <canvas ref={canvasRef} className="preview-image" aria-label="NR 预览" />
          </div>
          {exportLabel ? (
            <div className="preview-progress nr-progress">
              <ProgressBar
                aria-label="NR 导出进度"
                label={exportLabel}
                value={exportPercent}
                size="S"
                staticColor="white"
                styles={style({width: 'full'})}
              />
            </div>
          ) : null}
          <div className="nr-timeline">
            <ActionButton
              isQuiet
              aria-label={playing ? '暂停' : '播放'}
              isDisabled={!info || exporting}
              onPress={togglePlay}>
              <div slot="icon">{playing ? <Pause /> : <Play />}</div>
            </ActionButton>
            <Slider
              aria-label="帧"
              size="S"
              minValue={0}
              maxValue={Math.max(0, (info?.frames ?? 1) - 1)}
              step={1}
              value={frame}
              onChange={scrub}
              isDisabled={!info || exporting}
              styles={style({flexGrow: 1, minWidth: 0})}
            />
            <span className={`nr-frame-label ${style({font: 'detail-sm'})}`}>
              {info
                ? `${String(frame + 1).padStart(4, '0')} / ${String(info.frames).padStart(4, '0')}${
                    evalMs > 0 ? ` · ${Math.round(evalMs)}ms` : ''
                  }`
                : '— / —'}
            </span>
          </div>
        </div>
      </section>
    </div>
  );
}
