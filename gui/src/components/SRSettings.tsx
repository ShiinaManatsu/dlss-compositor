import React from 'react';
import { useConfig } from '../state/config-store';
import { DlssConfig } from '../types/dlss-config';
import { useIsRunning } from '../state/processing-store';
import { handleNumberInputKeyDown, flashInput } from './utils';
import ResolutionPanel from './ResolutionPanel';

export default function SRSettings() {
  const { state, dispatch } = useConfig();
  const isRunning = useIsRunning();

  const showValidationError = !state.scaleEnabled && state.interpolateFactor === 0;

  return (
    <div className="p-4 space-y-4 border-t border-gray-700">
      <div className="flex justify-between items-center mb-3">
        <h2 className="text-sm font-semibold text-gray-300 uppercase tracking-wider">
          SR Settings
        </h2>
        {/* Note: mode-validation-error is kept here since "Enable Upscaling" lives here */}
        {showValidationError && (
          <span data-testid="mode-validation-error" className="text-red-500 text-xs px-2 py-1 bg-red-500/10 border border-red-500/20 rounded">
            Select upscaling or interpolation
          </span>
        )}
      </div>

      {/* Upscaling Section */}
      <div className="space-y-4">
        <label className="flex items-center space-x-2">
          <input
            data-testid="scale-enabled"
            type="checkbox"
            checked={state.scaleEnabled}
            disabled={isRunning}
            onChange={(e) => dispatch({ type: 'SET_SCALE_ENABLED', payload: e.target.checked })}
            className="w-4 h-4 rounded bg-gray-700 border-gray-600 focus:ring-blue-500 disabled:opacity-50"
          />
          <span className="font-medium">Enable Upscaling</span>
        </label>

        <div className={`space-y-4 pl-6 ${!state.scaleEnabled ? 'hidden' : ''}`}>
          <div>
            <label className="block text-sm text-gray-400 mb-1">Scale Factor</label>
            <input
              data-testid="scale-input"
              type="number"
              min={1.0}
              max={8.0}
              step={0.1}
              disabled={isRunning}
              value={state.scaleFactor}
              onChange={(e) => dispatch({ type: 'SET_SCALE', payload: parseFloat(e.target.value) || 1.0 })}
              onKeyDown={handleNumberInputKeyDown}
              onBlur={(e) => {
                const val = parseFloat(e.target.value) || 2.0;
                const clamped = Math.max(1.0, Math.min(8.0, val));
                if (clamped !== state.scaleFactor) {
                  dispatch({ type: 'SET_SCALE', payload: clamped });
                  flashInput(e);
                }
              }}
              className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none transition-all disabled:opacity-50"
            />
          </div>

          <div>
            <label className="block text-sm text-gray-400 mb-1">Quality Mode</label>
            <select
              data-testid="quality-select"
              value={state.quality}
              disabled={isRunning}
              onChange={(e) => dispatch({ type: 'SET_QUALITY', payload: e.target.value as DlssConfig['quality'] })}
              className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
            >
              <option value="DLAA">DLAA</option>
              <option value="MaxQuality">MaxQuality</option>
              <option value="Balanced">Balanced</option>
              <option value="Performance">Performance</option>
              <option value="UltraPerformance">UltraPerformance</option>
            </select>
          </div>

          <div>
            <label className="block text-sm text-gray-400 mb-1">Preset</label>
            <select
              data-testid="preset-select"
              value={state.preset}
              disabled={isRunning}
              onChange={(e) => dispatch({ type: 'SET_PRESET', payload: e.target.value as DlssConfig['preset'] })}
              className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
            >
              <option value="J">Preset J</option>
              <option value="K">Preset K</option>
              <option value="L">Preset L (Default)</option>
              <option value="M">Preset M</option>
            </select>
          </div>
        </div>
      </div>

      <ResolutionPanel />

      {/* Source FPS Section */}
      <div className="space-y-2 pt-4 border-t border-gray-700">
        <div>
          <label className="block text-sm text-gray-400 mb-1" title="Animation frame rate of source footage (used by DLSS frame timing). Distinct from video encoding FPS.">
            Source FPS (animation frame rate)
          </label>
          <input
            data-testid="source-fps-input"
            type="number"
            min={1}
            max={240}
            value={state.sourceFps}
            disabled={isRunning}
            onChange={(e) => dispatch({ type: 'SET_SOURCE_FPS', payload: parseInt(e.target.value, 10) || 30 })}
            onKeyDown={handleNumberInputKeyDown}
            onBlur={(e) => {
              const val = parseInt(e.target.value, 10) || 30;
              const clamped = Math.max(1, Math.min(240, val));
              if (clamped !== state.sourceFps) {
                dispatch({ type: 'SET_SOURCE_FPS', payload: clamped });
                flashInput(e);
              }
            }}
            className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none transition-all disabled:opacity-50"
          />
        </div>
      </div>

      {/* Sharpness Section */}
      <div className="pt-4 border-t border-gray-700">
        <label className="block text-sm text-gray-400 mb-1" title="Post-DLSS sharpening (0=off, 0.2-0.4 recommended)">
          Sharpness (Post-DLSS)
        </label>
        <div className="flex items-center space-x-4">
          <input
            data-testid="sharpness-slider"
            type="range"
            min={0}
            max={1}
            step={0.05}
            disabled={isRunning}
            value={state.sharpness}
            onChange={(e) => dispatch({ type: 'SET_SHARPNESS', payload: parseFloat(e.target.value) })}
            className="flex-1 accent-blue-500 disabled:opacity-50"
          />
          <span className="w-10 text-sm text-center">{state.sharpness.toFixed(2)}</span>
        </div>
      </div>
    </div>
  );
}
