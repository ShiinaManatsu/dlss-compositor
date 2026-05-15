import React from 'react';
import { useConfig } from '../state/config-store';
import { DlssConfig } from '../types/dlss-config';
import { useIsRunning } from '../state/processing-store';

export default function FGSettings() {
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

  const handleForwardLutBrowse = async () => {
    try {
      const file = await window.dlssApi.selectFile([{ name: 'LUT', extensions: ['bin'] }]);
      if (file) {
        dispatch({ type: 'SET_FORWARD_LUT', payload: file });
      }
    } catch (err) {
      console.error(err);
    }
  };

  const handleInverseLutBrowse = async () => {
    try {
      const file = await window.dlssApi.selectFile([{ name: 'LUT', extensions: ['bin'] }]);
      if (file) {
        dispatch({ type: 'SET_INVERSE_LUT', payload: file });
      }
    } catch (err) {
      console.error(err);
    }
  };

  return (
    <div className="p-4 space-y-4 border-t border-gray-700">
      <h2 className="text-sm font-semibold text-gray-300 uppercase tracking-wider mb-3">
        FG Settings
      </h2>

      {/* Interpolation Section */}
      <div className="space-y-4">
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

      {/* Tonemapping */}
      <div className="space-y-4 border-t border-gray-700 pt-4">
        <div>
          <label className="block text-sm text-gray-400 mb-1" title="Tonemapping mode for Frame Generation transport">
            Tonemapping
          </label>
          <select
            data-testid="tonemap-select"
            value={state.tonemapMode}
            disabled={isRunning}
            onChange={(e) => dispatch({ type: 'SET_TONEMAP_MODE', payload: e.target.value as DlssConfig['tonemapMode'] })}
            className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
          >
            <option value="pq">pq (default)</option>
            <option value="none">none</option>
            <option value="custom">custom (LUT files)</option>
          </select>
        </div>

        <label className="flex items-center space-x-2" title="Skip inverse tonemap decode (output stays mapped)">
          <input
            data-testid="no-inverse-tonemap"
            type="checkbox"
            disabled={state.tonemapMode !== 'custom' || isRunning}
            checked={!state.inverseTonemapEnabled}
            onChange={(e) => dispatch({ type: 'SET_INVERSE_TONEMAP', payload: !e.target.checked })}
            className="w-4 h-4 rounded bg-gray-700 border-gray-600 focus:ring-blue-500 disabled:opacity-50 disabled:cursor-not-allowed"
          />
          <span className={`text-sm ${state.tonemapMode !== 'custom' ? 'opacity-50' : ''}`}>No Inverse Tonemap</span>
        </label>

        <div>
          <label className="block text-sm text-gray-400 mb-1" title="Custom 3D LUT file for forward mapping (required when tonemap=custom)">
            Forward LUT File
          </label>
          <div className="flex space-x-2">
            <input
              type="text"
              readOnly
              value={state.forwardLutFile}
              disabled={state.tonemapMode !== 'custom' || isRunning}
              className="flex-1 bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm disabled:opacity-50"
            />
            <button
              data-testid="forward-lut-picker"
              onClick={handleForwardLutBrowse}
              disabled={state.tonemapMode !== 'custom' || isRunning}
              className="bg-gray-700 hover:bg-gray-600 px-3 py-1.5 rounded text-sm transition-colors border border-gray-600 disabled:opacity-50 disabled:cursor-not-allowed"
            >
              Browse
            </button>
          </div>
          {state.tonemapMode === 'custom' && !state.forwardLutFile && (
            <span className="text-yellow-500 text-xs mt-1 block">⚠ Custom tonemap requires forward LUT file</span>
          )}
        </div>

        <label className="flex items-center space-x-2" title="Reserved FG-only knob (may not affect current SR processing)">
          <input
            data-testid="advanced-dynamic-resolution"
            type="checkbox"
            disabled={isRunning}
            checked={state.enableDynamicResolution}
            onChange={(e) => dispatch({ type: 'SET_ENABLE_DYNAMIC_RESOLUTION', payload: e.target.checked })}
            className="w-4 h-4 rounded bg-gray-700 border-gray-600 focus:ring-blue-500 disabled:opacity-50"
          />
          <span className="text-sm text-gray-400">Enable Dynamic Resolution (FG)</span>
        </label>

        <div>
          <label className="block text-sm text-gray-400 mb-1" title="Custom 3D LUT file for inverse mapping (.bin)">
            Inverse LUT File
          </label>
          <div className="flex space-x-2">
            <input
              type="text"
              readOnly
              value={state.inverseLutFile}
              disabled={state.tonemapMode !== 'custom' || !state.inverseTonemapEnabled || isRunning}
              className="flex-1 bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm disabled:opacity-50"
            />
            <button
              data-testid="inverse-lut-picker"
              onClick={handleInverseLutBrowse}
              disabled={state.tonemapMode !== 'custom' || !state.inverseTonemapEnabled || isRunning}
              className="bg-gray-700 hover:bg-gray-600 px-3 py-1.5 rounded text-sm transition-colors border border-gray-600 disabled:opacity-50 disabled:cursor-not-allowed"
            >
              Browse
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}
