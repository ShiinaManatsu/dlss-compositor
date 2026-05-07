import React from 'react';
import { useConfig } from '../state/config-store';
import { useIsRunning } from '../state/processing-store';

const PRESETS = [
  { label: '4K (3840×2160)', value: '4K' as const },
  { label: '1440p (2560×1440)', value: '1440p' as const },
  { label: '1080p (1920×1080)', value: '1080p' as const },
  { label: 'DLAA (native resolution)', value: 'DLAA' as const },
  { label: 'Custom...', value: 'Custom' as const },
];

export default function ResolutionPanel() {
  const { state, dispatch } = useConfig();
  const isRunning = useIsRunning();
  
  const sourceKnown = state.sourceWidth !== null && state.sourceHeight !== null;
  const sourceLabel = sourceKnown 
    ? `${state.sourceWidth} × ${state.sourceHeight}` 
    : 'Unknown (no camera.json)';
  
  return (
    <div className="space-y-4 pt-4 border-t border-gray-700">
      <div>
        <span className="block text-sm font-medium mb-2">Resolution</span>
        <div className="grid grid-cols-[1fr,auto,1fr] items-center gap-2">
          {/* Source */}
          <div className="text-center">
            <div className="text-xs text-gray-400 mb-1">Source</div>
            <div 
              data-testid="source-resolution-display"
              className={`bg-gray-800 border border-gray-600 rounded px-2 py-1.5 text-sm text-center ${!sourceKnown ? 'text-gray-500 italic' : ''}`}
            >
              {sourceLabel}
            </div>
          </div>
          
          {/* Arrow */}
          <div className="text-gray-500 text-lg">→</div>
          
          {/* Target */}
          <div className="text-center">
            <div className="text-xs text-gray-400 mb-1">Target</div>
            <select
              data-testid="target-preset-select"
              value={state.targetPreset}
              disabled={isRunning}
              onChange={(e) => dispatch({ type: 'SET_TARGET_PRESET', payload: e.target.value as any })}
              className="w-full bg-gray-700 border border-gray-600 rounded px-2 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
            >
              {PRESETS.map(p => (
                <option key={p.value} value={p.value}>{p.label}</option>
              ))}
            </select>
          </div>
        </div>
        
        {/* Custom WxH inputs */}
        {state.targetPreset === 'Custom' && (
          <div className="mt-2 flex space-x-2 items-center">
            <input
              data-testid="custom-target-width"
              type="number"
              min={1}
              placeholder="Width"
              disabled={isRunning}
              value={state.customTargetWidth ?? ''}
              onChange={(e) => {
                const w = parseInt(e.target.value, 10) || null;
                dispatch({ type: 'SET_CUSTOM_TARGET_RESOLUTION', payload: { width: w, height: state.customTargetHeight } });
              }}
              className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
            />
            <span className="text-gray-500">×</span>
            <input
              data-testid="custom-target-height"
              type="number"
              min={1}
              placeholder="Height"
              disabled={isRunning}
              value={state.customTargetHeight ?? ''}
              onChange={(e) => {
                const h = parseInt(e.target.value, 10) || null;
                dispatch({ type: 'SET_CUSTOM_TARGET_RESOLUTION', payload: { width: state.customTargetWidth, height: h } });
              }}
              className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
            />
          </div>
        )}
        
        {/* Scale display */}
        <div className="mt-2 text-xs text-gray-400 text-right">
          {!sourceKnown && state.targetPreset !== 'DLAA' && (
            <span className="text-yellow-500">⚠ Source unknown — enter manually or select input dir with camera.json</span>
          )}
          {(sourceKnown || state.targetPreset === 'DLAA') && (
            <span data-testid="computed-scale-display">Scale: {state.scaleFactor.toFixed(2)}×</span>
          )}
        </div>
      </div>
    </div>
  );
}
