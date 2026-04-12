import React, { useState } from 'react';
import { useConfig } from '../state/config-store';
import { DlssConfig } from '../types/dlss-config';
import { useIsRunning } from '../state/processing-store';
import { handleNumberInputKeyDown, flashInput } from './utils';

export default function AdvancedSettings() {
  const { state, dispatch } = useConfig();
  const isRunning = useIsRunning();
  const [open, setOpen] = useState(false);

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

  const handleChannelMapBrowse = async () => {
    try {
      const file = await window.dlssApi.selectFile([{ name: 'JSON', extensions: ['json'] }]);
      if (file) {
        dispatch({ type: 'SET_CHANNEL_MAP_FILE', payload: file });
      }
    } catch (err) {
      console.error(err);
    }
  };

  const togglePass = (pass: 'beauty' | 'depth' | 'normals', checked: boolean) => {
    let newPasses = [...state.outputPasses];
    if (checked && !newPasses.includes(pass)) {
      newPasses.push(pass);
    } else if (!checked && newPasses.includes(pass)) {
      newPasses = newPasses.filter(p => p !== pass);
    }
    dispatch({ type: 'SET_OUTPUT_PASSES', payload: newPasses });
  };

  const isDwaEnabled = state.exrCompression === 'dwaa' || state.exrCompression === 'dwab';

  return (
    <div className="p-4 space-y-4 border-t border-gray-700">
      <button
        data-testid="advanced-settings-toggle"
        onClick={() => setOpen(!open)}
        className="flex items-center text-sm font-medium text-gray-300 hover:text-white transition-colors"
      >
        Advanced Settings
        <span className={`ml-1 transition-transform ${open ? 'rotate-180' : 'rotate-0'}`}>▾</span>
      </button>

      <div data-testid="advanced-settings-content" className={`space-y-6 ${open ? '' : 'hidden'}`}>
        
        {/* Memory Budget */}
        <div>
          <label className="block text-sm text-gray-400 mb-1" title="GPU memory pool budget for processing (GB)">
            Memory Budget (GB)
          </label>
          <div className="flex items-center space-x-4">
            <input
              data-testid="memory-slider"
              type="range"
              min={1}
              max={32}
              step={1}
              disabled={isRunning}
              value={state.memoryBudgetGB}
              onChange={(e) => dispatch({ type: 'SET_MEMORY_BUDGET', payload: parseInt(e.target.value, 10) })}
              className="flex-1 accent-blue-500 disabled:opacity-50"
            />
            <input
              data-testid="memory-input"
              type="number"
              min={1}
              max={32}
              step={1}
              disabled={isRunning}
              value={state.memoryBudgetGB}
              onChange={(e) => dispatch({ type: 'SET_MEMORY_BUDGET', payload: parseInt(e.target.value, 10) || 8 })}
              onKeyDown={handleNumberInputKeyDown}
              onBlur={(e) => {
                const val = parseInt(e.target.value, 10) || 8;
                const clamped = Math.max(1, Math.min(32, val));
                if (clamped !== state.memoryBudgetGB) {
                  dispatch({ type: 'SET_MEMORY_BUDGET', payload: clamped });
                  flashInput(e);
                }
              }}
              className="w-20 bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none transition-all disabled:opacity-50"
            />
          </div>
        </div>

        {/* EXR Output */}
        <div className="space-y-4">
          <div>
            <label className="block text-sm text-gray-400 mb-1" title="Compression algorithm for output EXR files">
              EXR Compression
            </label>
            <select
              data-testid="exr-compression-select"
              value={state.exrCompression}
              disabled={isRunning}
              onChange={(e) => dispatch({ type: 'SET_EXR_COMPRESSION', payload: e.target.value as DlssConfig['exrCompression'] })}
              className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
            >
              <option value="none">none</option>
              <option value="zip">zip</option>
              <option value="zips">zips</option>
              <option value="piz">piz</option>
              <option value="dwaa">dwaa</option>
              <option value="dwab">dwab</option>
            </select>
          </div>

          <div>
            <label className="block text-sm text-gray-400 mb-1" title="DWA compression quality level (lower is more compressed)">
              DWA Quality
            </label>
            <input
              data-testid="dwa-quality-input"
              type="number"
              min={0}
              max={500}
              step={1}
              value={state.exrDwaQuality}
              disabled={!isDwaEnabled || isRunning}
              onChange={(e) => dispatch({ type: 'SET_EXR_DWA_QUALITY', payload: parseFloat(e.target.value) || 0 })}
              onKeyDown={handleNumberInputKeyDown}
              onBlur={(e) => {
                const val = parseFloat(e.target.value) || 45;
                const clamped = Math.max(0, Math.min(500, val));
                if (clamped !== state.exrDwaQuality) {
                  dispatch({ type: 'SET_EXR_DWA_QUALITY', payload: clamped });
                  flashInput(e);
                }
              }}
              className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none transition-all disabled:opacity-50 disabled:cursor-not-allowed"
            />
          </div>

          <div>
            <label className="block text-sm text-gray-400 mb-2" title="Which render passes to output">
              Output Passes
            </label>
            <div className="flex flex-col space-y-2">
              <label className="flex items-center space-x-2">
                <input
                  data-testid="pass-beauty"
                  type="checkbox"
                  disabled={isRunning}
                  checked={state.outputPasses.includes('beauty')}
                  onChange={(e) => togglePass('beauty', e.target.checked)}
                  className="w-4 h-4 rounded bg-gray-700 border-gray-600 focus:ring-blue-500 disabled:opacity-50"
                />
                <span className="text-sm">Beauty</span>
              </label>
              <label className="flex items-center space-x-2">
                <input
                  data-testid="pass-depth"
                  type="checkbox"
                  disabled={isRunning}
                  title="(experimental) Depth pass — may not be supported"
                  checked={state.outputPasses.includes('depth')}
                  onChange={(e) => togglePass('depth', e.target.checked)}
                  className="w-4 h-4 rounded bg-gray-700 border-gray-600 focus:ring-blue-500 disabled:opacity-50"
                />
                <span className="text-sm" title="(experimental) Depth pass — may not be supported">Depth (experimental)</span>
              </label>
              <label className="flex items-center space-x-2">
                <input
                  data-testid="pass-normals"
                  type="checkbox"
                  disabled={isRunning}
                  title="(experimental) Normals pass — may not be supported"
                  checked={state.outputPasses.includes('normals')}
                  onChange={(e) => togglePass('normals', e.target.checked)}
                  className="w-4 h-4 rounded bg-gray-700 border-gray-600 focus:ring-blue-500 disabled:opacity-50"
                />
                <span className="text-sm" title="(experimental) Normals pass — may not be supported">Normals (experimental)</span>
              </label>
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
              <option value="pq">pq</option>
              <option value="none">none</option>
            </select>
          </div>

          <label className="flex items-center space-x-2" title="Skip inverse tonemap decode (output stays mapped)">
            <input
              data-testid="no-inverse-tonemap"
              type="checkbox"
              disabled={state.tonemapMode === 'none' || isRunning}
              checked={!state.inverseTonemapEnabled}
              onChange={(e) => dispatch({ type: 'SET_INVERSE_TONEMAP', payload: !e.target.checked })}
              className="w-4 h-4 rounded bg-gray-700 border-gray-600 focus:ring-blue-500 disabled:opacity-50 disabled:cursor-not-allowed"
            />
            <span className={`text-sm ${state.tonemapMode === 'none' ? 'opacity-50' : ''}`}>No Inverse Tonemap</span>
          </label>

          <div>
            <label className="block text-sm text-gray-400 mb-1" title="Custom 3D LUT file for forward mapping (.bin)">
              Forward LUT File
            </label>
            <div className="flex space-x-2">
              <input
                type="text"
                readOnly
                value={state.forwardLutFile}
                disabled={state.tonemapMode === 'none' || isRunning}
                className="flex-1 bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm disabled:opacity-50"
              />
              <button
                data-testid="forward-lut-picker"
                onClick={handleForwardLutBrowse}
                disabled={state.tonemapMode === 'none' || isRunning}
                className="bg-gray-700 hover:bg-gray-600 px-3 py-1.5 rounded text-sm transition-colors border border-gray-600 disabled:opacity-50 disabled:cursor-not-allowed"
              >
                Browse
              </button>
            </div>
          </div>

          <div>
            <label className="block text-sm text-gray-400 mb-1" title="Custom 3D LUT file for inverse mapping (.bin)">
              Inverse LUT File
            </label>
            <div className="flex space-x-2">
              <input
                type="text"
                readOnly
                value={state.inverseLutFile}
                disabled={state.tonemapMode === 'none' || !state.inverseTonemapEnabled || isRunning}
                className="flex-1 bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm disabled:opacity-50"
              />
              <button
                data-testid="inverse-lut-picker"
                onClick={handleInverseLutBrowse}
                disabled={state.tonemapMode === 'none' || !state.inverseTonemapEnabled || isRunning}
                className="bg-gray-700 hover:bg-gray-600 px-3 py-1.5 rounded text-sm transition-colors border border-gray-600 disabled:opacity-50 disabled:cursor-not-allowed"
              >
                Browse
              </button>
            </div>
          </div>
        </div>

        {/* Custom Channel Map */}
        <div className="space-y-4 border-t border-gray-700 pt-4">
          <div>
            <label className="block text-sm text-gray-400 mb-1" title="Custom JSON mapping for EXR channels">
              Channel Map File
            </label>
            <div className="flex space-x-2">
              <input
                type="text"
                readOnly
                value={state.channelMapFile}
                disabled={isRunning}
                className="flex-1 bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm disabled:opacity-50"
              />
              <button
                data-testid="channel-map-picker"
                onClick={handleChannelMapBrowse}
                disabled={isRunning}
                className="bg-gray-700 hover:bg-gray-600 px-3 py-1.5 rounded text-sm transition-colors border border-gray-600 disabled:opacity-50 disabled:cursor-not-allowed"
              >
                Browse
              </button>
            </div>
          </div>
        </div>

      </div>
    </div>
  );
}
