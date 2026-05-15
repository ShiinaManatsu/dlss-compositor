import React from 'react';
import { useConfig } from '../state/config-store';
import { useIsRunning } from '../state/processing-store';
import { handleNumberInputKeyDown, flashInput } from './utils';
import { DlssConfig } from '../types/dlss-config';

export default function CommonSettings() {
  const { state, dispatch } = useConfig();
  const isRunning = useIsRunning();

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
      <h2 className="text-sm font-semibold text-gray-300 uppercase tracking-wider mb-3">
        Common Settings
      </h2>
      
      {/* Output Section (Video) */}
      <div className="space-y-4">
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

      {/* Memory Budget */}
      <div className="pt-4 border-t border-gray-700">
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

      {/* NGX Log Level */}
      <div className="pt-4 border-t border-gray-700">
        <label className="block text-sm text-gray-400 mb-1" title="NGX SDK log verbosity (may slow processing at verbose)">
          NGX SDK Log Verbosity
        </label>
        <select
          data-testid="ngx-log-level-select"
          value={state.ngxLogLevel}
          disabled={isRunning}
          onChange={(e) => dispatch({ type: 'SET_NGX_LOG_LEVEL', payload: e.target.value as DlssConfig['ngxLogLevel'] })}
          className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
        >
          <option value="off">off</option>
          <option value="error">error</option>
          <option value="warn">warn (default)</option>
          <option value="info">info</option>
          <option value="verbose">verbose</option>
        </select>
      </div>

      {/* EXR Output */}
      <div className="space-y-4 border-t border-gray-700 pt-4">
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
  );
}
