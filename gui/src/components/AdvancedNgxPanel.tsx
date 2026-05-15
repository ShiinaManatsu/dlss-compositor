import React from 'react';
import { useConfig } from '../state/config-store';
import { DlssConfig } from '../types/dlss-config';
import { useIsRunning } from '../state/processing-store';

const PRESET_OPTIONS: DlssConfig['presetDLAA'][] = ['auto', 'J', 'K', 'L', 'M'];

const PRESET_TOOLTIP =
  'Auto uses NVIDIA recommended: K for DLAA/Quality/Balanced/UltraQuality, M for Performance, L for UltraPerformance';

type PresetField =
  | 'presetDLAA'
  | 'presetQuality'
  | 'presetBalanced'
  | 'presetPerformance'
  | 'presetUltraPerformance'
  | 'presetUltraQuality';

type PresetAction =
  | 'SET_PRESET_DLAA'
  | 'SET_PRESET_QUALITY'
  | 'SET_PRESET_BALANCED'
  | 'SET_PRESET_PERFORMANCE'
  | 'SET_PRESET_ULTRA_PERFORMANCE'
  | 'SET_PRESET_ULTRA_QUALITY';

const PRESET_ROWS: ReadonlyArray<readonly [string, PresetField, PresetAction]> = [
  ['DLAA', 'presetDLAA', 'SET_PRESET_DLAA'],
  ['Quality', 'presetQuality', 'SET_PRESET_QUALITY'],
  ['Balanced', 'presetBalanced', 'SET_PRESET_BALANCED'],
  ['Performance', 'presetPerformance', 'SET_PRESET_PERFORMANCE'],
  ['UltraPerf', 'presetUltraPerformance', 'SET_PRESET_ULTRA_PERFORMANCE'],
  ['UltraQuality', 'presetUltraQuality', 'SET_PRESET_ULTRA_QUALITY'],
];

type FlagField =
  | 'featureFlagHDR'
  | 'featureFlagMVLowRes'
  | 'featureFlagAutoExposure'
  | 'enableOutputSubrects';

type FlagAction =
  | 'SET_FEATURE_FLAG_HDR'
  | 'SET_FEATURE_FLAG_MV_LOW_RES'
  | 'SET_FEATURE_FLAG_AUTO_EXPOSURE'
  | 'SET_ENABLE_OUTPUT_SUBRECTS';

const FLAG_ROWS: ReadonlyArray<readonly [FlagField, FlagAction, string, string]> = [
  ['featureFlagHDR', 'SET_FEATURE_FLAG_HDR', 'HDR input (IsHDR)', 'Treat input as HDR — recommended for EXR scenes'],
  ['featureFlagMVLowRes', 'SET_FEATURE_FLAG_MV_LOW_RES', 'Render-res MV (MVLowRes)', 'Motion vectors are at render resolution (Blender default)'],
  ['featureFlagAutoExposure', 'SET_FEATURE_FLAG_AUTO_EXPOSURE', 'Auto-exposure (AutoExposure)', 'NGX auto-handles exposure — disable if using custom exposure-scale'],
  ['enableOutputSubrects', 'SET_ENABLE_OUTPUT_SUBRECTS', 'Output subrects', 'Enable sub-rectangle output support'],
];

export default function AdvancedNgxPanel() {
  const { state, dispatch } = useConfig();
  const isRunning = useIsRunning();

  return (
    <details className="border border-gray-700 rounded">
      <summary
        data-testid="advanced-ngx-toggle"
        className="cursor-pointer p-3 text-sm font-medium text-gray-300 hover:text-white transition-colors select-none"
      >
        Advanced NGX Settings (Power User)
      </summary>

      <div data-testid="advanced-ngx-content" className="p-3 space-y-4 border-t border-gray-700">

        {/* Per-quality presets */}
        <div>
          <div className="flex items-center gap-1 mb-2">
            <span className="text-xs font-medium text-gray-400">Per-Quality Presets</span>
            <span className="text-gray-600" title={PRESET_TOOLTIP}>ⓘ</span>
          </div>
          <div className="grid grid-cols-2 gap-2">
            {PRESET_ROWS.map(([label, field, action]) => (
              <div key={field}>
                <label className="block text-xs text-gray-500 mb-0.5">{label}</label>
                <select
                  data-testid={`advanced-preset-${field}`}
                  value={state[field]}
                  disabled={isRunning}
                  onChange={(e) => dispatch({
                    type: action,
                    payload: e.target.value as DlssConfig['presetDLAA'],
                  } as never)}
                  className="w-full bg-gray-800 border border-gray-600 rounded px-2 py-1 text-xs focus:ring-1 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
                >
                  {PRESET_OPTIONS.map((o) => (
                    <option key={o} value={o}>
                      {o === 'auto' ? 'Auto (NVIDIA)' : `Preset ${o}`}
                    </option>
                  ))}
                </select>
              </div>
            ))}
          </div>
        </div>

        {/* Feature flags */}
        <div>
          <span className="text-xs font-medium text-gray-400 block mb-2">Feature Flags</span>
          <div className="space-y-1.5">
            {FLAG_ROWS.map(([field, action, label, tooltip]) => (
              <label key={field} className="flex items-center gap-2" title={tooltip}>
                <input
                  data-testid={`advanced-flag-${field}`}
                  type="checkbox"
                  disabled={isRunning}
                  checked={state[field]}
                  onChange={(e) => dispatch({
                    type: action,
                    payload: e.target.checked,
                  } as never)}
                  className="w-3.5 h-3.5 rounded bg-gray-700 border-gray-600 focus:ring-blue-500 disabled:opacity-50"
                />
                <span className="text-xs text-gray-300">{label}</span>
              </label>
            ))}
          </div>
        </div>

        {/* Numeric overrides */}
        <div>
          <span className="text-xs font-medium text-gray-400 block mb-2">Eval Overrides</span>
          <div className="space-y-2">
            <div>
              <label className="text-xs text-gray-500" title="InExposureScale — only meaningful when Auto-Exposure is OFF">
                Exposure Scale <span className="text-gray-600">(default: 1.0)</span>
              </label>
              <input
                data-testid="advanced-exposure-scale"
                type="number" min={0.01} max={100} step={0.01}
                disabled={isRunning}
                value={state.exposureScale}
                onChange={(e) => dispatch({ type: 'SET_EXPOSURE_SCALE', payload: parseFloat(e.target.value) || 1.0 })}
                className="w-full mt-0.5 bg-gray-800 border border-gray-600 rounded px-2 py-1 text-xs focus:ring-1 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
              />
            </div>
            <div>
              <label className="text-xs text-gray-500" title="Override frame time delta in ms. -1 = auto from --source-fps">
                Frame Time Delta Override (ms) <span className="text-gray-600">(-1 = auto)</span>
              </label>
              <input
                data-testid="advanced-frame-time-delta"
                type="number" min={-1} step={0.1}
                disabled={isRunning}
                value={state.frameTimeDeltaMsOverride}
                onChange={(e) => dispatch({ type: 'SET_FRAME_TIME_DELTA_MS', payload: parseFloat(e.target.value) })}
                className="w-full mt-0.5 bg-gray-800 border border-gray-600 rounded px-2 py-1 text-xs focus:ring-1 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
              />
            </div>
            <div>
              <label className="text-xs text-gray-500" title="NGX InSharpness [0,1]. Distinct from post-DLSS CAS sharpening above.">
                DLSS Sharpness (NGX InSharpness) <span className="text-gray-600">(default: 0.0)</span>
              </label>
              <input
                data-testid="advanced-dlss-sharpness"
                type="number" min={0} max={1} step={0.05}
                disabled={isRunning}
                value={state.dlssSharpness}
                onChange={(e) => dispatch({ type: 'SET_DLSS_SHARPNESS', payload: parseFloat(e.target.value) })}
                className="w-full mt-0.5 bg-gray-800 border border-gray-600 rounded px-2 py-1 text-xs focus:ring-1 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
              />
            </div>
          </div>
        </div>

        {/* Camera cut detection */}
        <div>
          <span className="text-xs font-medium text-gray-400 block mb-2">Camera Cut Detection</span>
          <div className="space-y-2">
            <label className="flex items-center gap-2" title="Detect camera cuts from geometry delta; fires InReset on cut frames">
              <input
                data-testid="advanced-camera-cut-enabled"
                type="checkbox"
                disabled={isRunning}
                checked={state.cameraCutDetectionEnabled}
                onChange={(e) => dispatch({ type: 'SET_CAMERA_CUT_DETECTION_ENABLED', payload: e.target.checked })}
                className="w-3.5 h-3.5 rounded bg-gray-700 border-gray-600 focus:ring-blue-500 disabled:opacity-50"
              />
              <span className="text-xs text-gray-300">Enable camera cut detection</span>
            </label>
            {state.cameraCutDetectionEnabled && (
              <div className="pl-5 space-y-1.5">
                <div>
                  <label className="text-xs text-gray-500">Translation threshold (world units, default: 5.0)</label>
                  <input
                    data-testid="advanced-cut-translation"
                    type="number" min={0.1} step={0.5}
                    disabled={isRunning}
                    value={state.cameraCutTranslationThreshold}
                    onChange={(e) => dispatch({ type: 'SET_CAMERA_CUT_TRANSLATION_THRESHOLD', payload: parseFloat(e.target.value) || 5.0 })}
                    className="w-full mt-0.5 bg-gray-800 border border-gray-600 rounded px-2 py-1 text-xs focus:ring-1 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
                  />
                </div>
                <div>
                  <label className="text-xs text-gray-500">Rotation threshold (degrees, default: 30.0)</label>
                  <input
                    data-testid="advanced-cut-rotation"
                    type="number" min={1} max={180} step={1}
                    disabled={isRunning}
                    value={state.cameraCutRotationThreshold}
                    onChange={(e) => dispatch({ type: 'SET_CAMERA_CUT_ROTATION_THRESHOLD', payload: parseFloat(e.target.value) || 30.0 })}
                    className="w-full mt-0.5 bg-gray-800 border border-gray-600 rounded px-2 py-1 text-xs focus:ring-1 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
                  />
                </div>
              </div>
            )}
          </div>
        </div>

      </div>
    </details>
  );
}
