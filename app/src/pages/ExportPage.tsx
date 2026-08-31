import type {ReactNode} from 'react';
import {
  ActionButton,
  Button,
  Checkbox,
  Disclosure,
  DisclosurePanel,
  DisclosureTitle,
  Divider,
  NumberField,
  Picker,
  PickerItem,
  ProgressBar,
  Switch,
  Text,
  TextField
} from '@react-spectrum/s2';
import FolderOpen from '@react-spectrum/s2/icons/FolderOpen';
import Refresh from '@react-spectrum/s2/icons/Refresh';
import {useApp, engineLabel} from '../appState';
import {
  FG_OPTIONS,
  OUTPUT_FORMATS,
  QUALITY_OPTIONS,
  RECOMMENDED_PERCENT,
  RR_QUALITY_IDS,
  RR_RECOMMENDED_PERCENT,
  cyclesDenoiseMeta,
  cyclesDeviceMeta,
  formatLabel,
  rrQualityAllowed,
} from '../dcp';
import {userErrorMessage} from '../errors';
import {style} from '@react-spectrum/s2/style' with {type: 'macro'};

function SettingsSection({
  title,
  defaultExpanded,
  children
}: {
  title: string;
  defaultExpanded?: boolean;
  children: ReactNode;
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

export function ExportPage() {
  const app = useApp();
  const {
    blend,
    setBlend,
    scene,
    hasScene,
    samplingLocked,
    canRR,
    inspecting,
    inspectError,
    inspectBlend,
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
    nativeResBuffers,
    setNativeResBuffers,
    nativeResEnabled,
    nrBuffer,
    setNrBuffer,
    fgEnabled,
    fgMultiplier,
    setFg,
    rrLocksDenoise,
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
    busy,
    message,
    error,
    displayFrame,
    frameEnd: jobFrameEnd,
    frameStart: jobFrameStart,
    outputFrames,
    progressPercent,
    etaText,
    registerPreviewCanvas,
    ngxWarning,
    probe,
    refreshProbe
  } = app;

  const ramOk = probe?.ramdisk.ok;
  const imdiskOk = probe?.imdisk.ok;
  const qualityOkForMode = mode !== 'rr' || rrQualityAllowed(quality);
  const rrRecPercent = RR_RECOMMENDED_PERCENT[quality] ?? RECOMMENDED_PERCENT[quality] ?? 58;
  const denoiseSummary = rrLocksDenoise
    ? 'RR · 关'
    : scene?.cycles
      ? scene.cycles.use_denoising
        ? `开 · ${scene.cycles.denoiser_label || scene.cycles.denoiser}`
        : '关'
      : '—';
  const sceneDeviceMeta = cyclesDeviceMeta(scene?.cycles?.device, scene?.cycles_compute?.compute_device_type);
  const sceneDenoiseMeta = cyclesDenoiseMeta(scene?.cycles);

  const progressLabel = error
    ? error
    : busy
      ? `第 ${String(displayFrame).padStart(4, '0')} / ${String(jobFrameEnd).padStart(4, '0')} 帧${
          etaText ? ` · ${etaText}` : ''
        }`
      : message && message !== '就绪'
        ? message
        : `共 ${outputFrames} 帧（Blender ${jobFrameStart}–${jobFrameEnd}） · ${formatLabel(outputFormat)} · ${outputWidth}×${outputHeight}`;

  return (
    <div className="export-body">
      <aside className="export-settings">
        <SettingsSection title="Blend 文件" defaultExpanded>
          <div className="field-with-action">
            <TextField label="Blend" size="S" value={blend} onChange={setBlend} isDisabled={busy} />
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
                  isDisabled={busy}
                  styles={style({width: 'full'})}>
                  <PickerItem id="OPENIMAGEDENOISE">OIDN</PickerItem>
                  <PickerItem id="OPTIX">OptiX</PickerItem>
                </Picker>
              ) : null}
            </>
          )}
        </SettingsSection>
        <Divider size="S" />
        <SettingsSection title="DLSS" defaultExpanded>
          <Picker
            label="模式"
            size="S"
            value={mode}
            onChange={(value) => setMode(String(value))}
            isDisabled={busy}
            styles={style({width: 'full'})}>
            <PickerItem id="sr">SR 超分辨率</PickerItem>
            <PickerItem id="rr" isDisabled={!canRR}>
              RR 光线重建
            </PickerItem>
          </Picker>
          <Picker
            label="质量"
            size="S"
            value={quality}
            onChange={(value) => setQuality(String(value))}
            isDisabled={busy}
            styles={style({width: 'full'})}>
            {QUALITY_OPTIONS.filter(
              (item) => mode !== 'rr' || (RR_QUALITY_IDS as readonly string[]).includes(item.id),
            ).map((item) => (
              <PickerItem key={item.id} id={item.id}>
                {item.label}
              </PickerItem>
            ))}
          </Picker>
          {mode === 'rr' ? (
            qualityOkForMode ? (
              <span className={style({font: 'detail-sm', color: 'gray-600'})}>
                RR 仅支持质量档：{quality} → {rrRecPercent}%（NGX RR 无超高质量/超高性能）
              </span>
            ) : (
              <span className={style({font: 'detail-sm', color: 'red-700'})}>
                {quality} 不被 RR 支持（NGX RR 无此档），请改选 DLAA / 高质量 / 平衡 / 性能
              </span>
            )
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
              <span className={style({font: 'detail-sm', color: 'gray-600'})}>
                推荐 {RECOMMENDED_PERCENT[quality] ?? 58}%
              </span>
              {ngxWarning && pins.percentage ? (
                <span className={style({font: 'detail-sm', color: 'red-700'})}>{ngxWarning}</span>
              ) : null}
            </>
          )}
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
            value={fgEnabled && fgMultiplier >= 2 && !nrBuffer ? `${fgMultiplier}x` : 'off'}
            onChange={(value) => setFg(value === 'off' ? 0 : Number(String(value).replace('x', '')))}
            isDisabled={!fgEnabled || busy || nrBuffer}
            styles={style({width: 'full'})}>
            {FG_OPTIONS.map((item) => (
              <PickerItem key={item.id} id={item.id}>
                {item.label}
              </PickerItem>
            ))}
          </Picker>
          <Switch
            size="S"
            isSelected={nrBuffer}
            onChange={setNrBuffer}
            isDisabled={busy}>
            导出 NR 引导缓冲
          </Switch>
          <span className={style({font: 'detail-sm', color: 'gray-600'})}>
            {nrBuffer
              ? 'dlssnrbuffer_####.exr（运动+深度）将写入输出目录，帧生成已强制关闭；导出后到 NR 工具页调参'
              : '供 NR 工具页使用的运动+深度数据（开启时帧生成关闭）'}
          </span>
          <span className={style({font: 'detail-sm', color: 'gray-600'})}>
            色彩 {hasScene ? scene?.view_transform || '—' : '—'}
          </span>
        </SettingsSection>
        <Divider size="S" />
        <SettingsSection title="导出" defaultExpanded>
          <div className="row-2">
            <NumberField label="宽" size="S" value={outputWidth} onChange={setOutputWidth} hideStepper isDisabled={busy} />
            <NumberField label="高" size="S" value={outputHeight} onChange={setOutputHeight} hideStepper isDisabled={busy} />
          </div>
          <Picker
            label="编码"
            size="S"
            value={outputFormat}
            onChange={(value) => setOutputFormat(String(value))}
            isDisabled={busy}
            styles={style({width: 'full'})}>
            {OUTPUT_FORMATS.map((item) => (
              <PickerItem key={item.id} id={item.id}>
                {item.label}
              </PickerItem>
            ))}
          </Picker>
          <TextField label="文件名模板" size="S" value={filenameTemplate} onChange={setFilenameTemplate} isDisabled={busy} />
          <span className={style({font: 'detail-sm', color: 'gray-600'})}>
            v{outputVersion} · {'{job} {version} {frame} {mode}'}
          </span>
          <div className="field-with-action">
            <TextField label="目录" size="S" value={exportDir} onChange={setExportDir} isDisabled={busy} />
            <ChooseDirButton onPick={setExportDir} isDisabled={busy} />
          </div>
          {elevated ? (
            <Switch size="S" isSelected={useRamdisk} onChange={setUseRamdisk} isDisabled={busy}>
              使用内存盘
            </Switch>
          ) : null}
          {elevated && useRamdisk ? (
            <>
              <div className="probe-line">
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
                <ActionButton size="S" aria-label="刷新探测" onPress={() => refreshProbe()}>
                  <Refresh />
                </ActionButton>
              </div>
              <span className={style({font: 'detail-sm', color: 'gray-600'})}>
                {ramOk
                  ? `${probe?.ramdisk.letter}: ${probe?.ramdisk.label || 'DLSSRAM'}`
                  : '未挂载'}
              </span>
            </>
          ) : (
            <div className="field-with-action">
              <TextField
                label="缓存路径"
                size="S"
                value={cachePath}
                onChange={setCachePath}
                isDisabled={busy}
              />
              <ChooseDirButton onPick={setCachePath} isDisabled={busy} />
            </div>
          )}
        </SettingsSection>
        <Divider size="S" />
        <SettingsSection title="高级">
          <NumberField
            label="消费窗口"
            size="S"
            minValue={4}
            maxValue={8}
            value={windowSize}
            onChange={setWindowSize}
            isDisabled={busy}
          />
          {elevated && useRamdisk ? (
            <NumberField
              label="内存盘大小 (GB)"
              size="S"
              minValue={1}
              maxValue={64}
              value={ramdiskGb}
              onChange={setRamdiskGb}
              isDisabled={busy}
              hideStepper
            />
          ) : null}
        </SettingsSection>
      </aside>

      <section className="preview-col">
        <div className="preview-well">
          <div className="preview-frame">
            <canvas ref={registerPreviewCanvas} className="preview-image" aria-label="当前预览帧" />
          </div>
          <div className="preview-progress">
            <ProgressBar
              aria-label="导出进度"
              label={progressLabel}
              value={progressPercent}
              size="S"
              staticColor="white"
              styles={style({width: 'full'})}
            />
          </div>
        </div>
      </section>
    </div>
  );
}

function ChooseDirButton({
  onPick,
  isDisabled
}: {
  onPick: (dir: string) => void;
  isDisabled?: boolean;
}) {
  return (
    <Button
      variant="secondary"
      size="S"
      isDisabled={isDisabled}
      onPress={() => {
        void window.dlss.chooseExportDir().then((picked) => {
          if (picked) onPick(picked);
        });
      }}>
      <FolderOpen />
      <Text>选择</Text>
    </Button>
  );
}
