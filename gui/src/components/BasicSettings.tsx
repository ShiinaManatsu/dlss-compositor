import React from 'react';
import { useConfig } from '../state/config-store';
import { DlssConfig } from '../types/dlss-config';
import { useIsRunning } from '../state/processing-store';
import { handleNumberInputKeyDown, flashInput } from './utils';

export default function BasicSettings() {
  const { state, dispatch } = useConfig();
  const isRunning = useIsRunning();

  const handleCameraDataBrowse = async () => {
    try {
      const file = await window.dlssApi.selectFile([{ name: 'JSON', extensions: ['json'] }]);
      if (file) {
        dispatch({ type: 'SET_CAMERA_DATA_FILE', payload: file });
      }
    } catch (err) {
      console.error(err);
    }
  };

  const showValidationError = !state.scaleEnabled && state.interpolateFactor === 0;

  return (
    <div className="p-4 space-y-6">
      {/* Validation Error */}
      {showValidationError && (
        <div data-testid="mode-validation-error" className="text-red-500 text-sm p-3 bg-red-500/10 border border-red-500/20 rounded">
          Select at least upscaling or interpolation
        </div>
      )}

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

      {/* Interpolation Section */}
      <div className="space-y-4 pt-4 border-t border-gray-700">
        <div>
          <label className="block font-medium mb-2">Interpolation</label>
          <select
            data-testid="interpolation-select"
            value={state.interpolateFactor}
            disabled={isRunning}
            onChange={(e) => dispatch({ type: 'SET_INTERPOLATION', payload: parseInt(e.target.value, 10) as 0 | 2 | 4 })}
            className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
          >
            <option value={0}>None</option>
            <option value={2}>2x</option>
            <option value={4}>4x</option>
          </select>
        </div>

        <div>
          <label className="block text-sm text-gray-400 mb-1">Camera Data File</label>
          <div className="flex space-x-2">
            <input
              type="text"
              readOnly
              value={state.cameraDataFile}
              disabled={isRunning || state.interpolateFactor === 0}
              className="flex-1 bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm disabled:opacity-50"
            />
            <button
              data-testid="camera-data-picker"
              onClick={handleCameraDataBrowse}
              disabled={isRunning || state.interpolateFactor === 0}
              className="bg-gray-700 hover:bg-gray-600 px-3 py-1.5 rounded text-sm transition-colors border border-gray-600 disabled:opacity-50 disabled:cursor-not-allowed"
            >
              Browse
            </button>
          </div>
        </div>
      </div>

      {/* Output Section */}
      <div className="space-y-4 pt-4 border-t border-gray-700">
        <label className="flex items-center space-x-2">
          <input
            data-testid="encode-video"
            type="checkbox"
            checked={state.encodeVideo}
            disabled={isRunning}
            onChange={(e) => dispatch({ type: 'SET_ENCODE_VIDEO', payload: e.target.checked })}
            className="w-4 h-4 rounded bg-gray-700 border-gray-600 focus:ring-blue-500 disabled:opacity-50"
          />
          <span className="font-medium">Encode Video</span>
        </label>

        {state.encodeVideo && (
          <div className="space-y-4 pl-6">
            <div>
              <label className="block text-sm text-gray-400 mb-1">FPS</label>
              <input
                data-testid="fps-input"
                type="number"
                min={1}
                max={240}
                value={state.fps}
                disabled={isRunning}
                onChange={(e) => dispatch({ type: 'SET_FPS', payload: parseInt(e.target.value, 10) || 24 })}
                onKeyDown={handleNumberInputKeyDown}
                onBlur={(e) => {
                  const val = parseInt(e.target.value, 10) || 24;
                  const clamped = Math.max(1, Math.min(240, val));
                  if (clamped !== state.fps) {
                    dispatch({ type: 'SET_FPS', payload: clamped });
                    flashInput(e);
                  }
                }}
                className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none transition-all disabled:opacity-50"
              />
            </div>
            <div>
              <label className="block text-sm text-gray-400 mb-1">Video Output File</label>
              <input
                data-testid="video-output-file"
                type="text"
                value={state.videoOutputFile}
                disabled={isRunning}
                onChange={(e) => dispatch({ type: 'SET_VIDEO_OUTPUT_FILE', payload: e.target.value })}
                className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
              />
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
