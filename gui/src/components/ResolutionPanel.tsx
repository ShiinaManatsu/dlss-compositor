import React, { useState, useEffect } from 'react';
import { useConfig } from '../state/config-store';
import { useIsRunning } from '../state/processing-store';

type Orientation = 'landscape' | 'portrait' | 'square' | 'unknown';

interface Preset {
  label: string;
  w: number;
}

const getPresets = (orientation: Orientation, sourceWidth: number): Preset[] => {
  if (orientation === 'portrait') {
    return [
      { label: 'Same as Source', w: sourceWidth },
      { label: '1080p (1080×1920)', w: 1080 },
      { label: '1440p (1440×2560)', w: 1440 },
      { label: '4K (2160×3840)', w: 2160 },
    ];
  } else if (orientation === 'square') {
    return [
      { label: 'Same as Source', w: sourceWidth },
      { label: '1080² (1080×1080)', w: 1080 },
      { label: '1440² (1440×1440)', w: 1440 },
      { label: '4K² (2160×2160)', w: 2160 },
    ];
  } else {
    // landscape and fallback
    return [
      { label: 'Same as Source', w: sourceWidth },
      { label: '1080p (1920×1080)', w: 1920 },
      { label: '1440p (2560×1440)', w: 2560 },
      { label: '4K (3840×2160)', w: 3840 },
    ];
  }
};

export default function ResolutionPanel() {
  const { state, dispatch } = useConfig();
  const isRunning = useIsRunning();
  
  const sourceKnown = state.sourceWidth != null && state.sourceHeight != null && 
                      state.sourceWidth > 0 && state.sourceHeight > 0;
  const aspect = sourceKnown ? state.sourceWidth! / state.sourceHeight! : 1.0;
  
  const orientation: Orientation = !sourceKnown ? 'unknown' :
                                   aspect > 1.01 ? 'landscape' :
                                   aspect < 0.99 ? 'portrait' : 'square';

  const [primaryDim, setPrimaryDim] = useState<'width' | 'height'>('width');
  const [targetWidthStr, setTargetWidthStr] = useState<string>('');
  const [scaleStr, setScaleStr] = useState<string>('');
  const [preset, setPreset] = useState<string>('Custom');

  // Initialization when source becomes known
  useEffect(() => {
    if (sourceKnown && state.sourceWidth! > 0) {
      const initW = Math.floor(state.sourceWidth! * 2);
      setTargetWidthStr(String(initW));
      setScaleStr('2.00');
      dispatch({ type: 'SET_SCALE', payload: 2.0 });
      setPreset('Custom');
    } else {
      setTargetWidthStr('');
      setScaleStr('');
      setPreset('Custom');
    }
    // Only re-run when source dimensions change (as requested)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state.sourceWidth, state.sourceHeight]);

  const handleWidthChange = (val: string) => {
    setPreset('Custom');
    setTargetWidthStr(val);
    const w = parseInt(val, 10);
    if (!isNaN(w) && w > 0 && sourceKnown && state.sourceWidth! > 0) {
      const scale = w / state.sourceWidth!;
      setScaleStr(scale.toFixed(2));
      dispatch({ type: 'SET_SCALE', payload: Math.round(scale * 100) / 100 });
    }
  };

  const handleHeightChange = (val: string) => {
    setPreset('Custom');
    const h = parseInt(val, 10);
    if (!isNaN(h) && h > 0 && sourceKnown) {
      const w = Math.floor(h * aspect);
      setTargetWidthStr(String(w));
      const scale = w / state.sourceWidth!;
      setScaleStr(scale.toFixed(2));
      dispatch({ type: 'SET_SCALE', payload: Math.round(scale * 100) / 100 });
    } else if (val === '') {
      setTargetWidthStr('');
    }
  };

  const handleScaleChange = (val: string) => {
    setPreset('Custom');
    setScaleStr(val);
    const scale = parseFloat(val);
    if (!isNaN(scale) && scale > 0) {
      if (sourceKnown && state.sourceWidth! > 0) {
        const w = Math.floor(state.sourceWidth! * scale);
        setTargetWidthStr(String(w));
      }
      dispatch({ type: 'SET_SCALE', payload: Math.round(scale * 100) / 100 });
    }
  };

  const handlePresetSelect = (presetLabel: string, w: number) => {
    setPreset(presetLabel);
    setTargetWidthStr(String(w));
    if (sourceKnown && state.sourceWidth! > 0) {
      const scale = w / state.sourceWidth!;
      setScaleStr(scale.toFixed(2));
      dispatch({ type: 'SET_SCALE', payload: Math.round(scale * 100) / 100 });
    }
  };

  // Derived values for display
  const targetWidth = parseInt(targetWidthStr, 10) || 0;
  const targetHeight = targetWidth > 0 ? Math.floor(targetWidth / aspect) : 0;
  const displayHeightStr = targetWidthStr === '' ? '' : String(targetHeight);

  return (
    <div className="p-4 border-t border-gray-700 space-y-4">
      <span className="block text-sm font-medium">Output Resolution</span>
      
      {!sourceKnown ? (
        <div className="space-y-3">
          <span data-testid="source-unknown-warning" className="text-xs text-yellow-400 block">
            ⚠ Source resolution unknown. Select an input directory with EXR files to enable aspect-locked mode.
          </span>
          <div className="flex items-center space-x-2">
            <label className="text-sm text-gray-300">Scale Factor:</label>
            <input
              data-testid="resolution-scale-input-fallback"
              type="number"
              min={0.5} max={8} step={0.01}
              disabled={isRunning}
              value={state.scaleFactor}
              onChange={(e) => {
                const val = parseFloat(e.target.value);
                if (!isNaN(val) && val > 0) {
                  dispatch({ type: 'SET_SCALE', payload: Math.round(val * 100) / 100 });
                }
              }}
              className="w-24 bg-gray-700 border border-gray-600 rounded px-2 py-1 text-sm focus:ring-2 focus:ring-blue-500"
            />
          </div>
        </div>
      ) : (
        <>
          <div className="text-sm">
            <span className="text-gray-400">Source: </span>
            <span className="font-medium text-gray-100">{state.sourceWidth} × {state.sourceHeight}</span>
            <span className="text-gray-500 ml-2">({aspect.toFixed(2)}:1)</span>
          </div>

          <div>
            <select
              data-testid="resolution-preset-select"
              value={preset}
              disabled={isRunning}
              onChange={(e) => {
                const label = e.target.value;
                if (label === 'Custom') {
                  setPreset('Custom');
                  return;
                }
                const presets = getPresets(orientation, state.sourceWidth!);
                const selected = presets.find(p => p.label === label);
                if (selected) {
                  handlePresetSelect(selected.label, selected.w);
                }
              }}
              className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
            >
              <option value="Custom">Custom...</option>
              <optgroup label="Presets">
                {getPresets(orientation, state.sourceWidth!).map(p => (
                  <option key={p.label} value={p.label}>{p.label}</option>
                ))}
              </optgroup>
            </select>
          </div>

          <div className="flex items-center space-x-3 bg-gray-800 p-3 rounded border border-gray-700">
            <div className="flex-1">
              <label className="block text-xs text-gray-400 mb-1 capitalize">{primaryDim}</label>
              <div className="flex space-x-2">
                <input
                  data-testid="primary-dim-input"
                  type="number"
                  min={1} step={1}
                  disabled={isRunning}
                  value={primaryDim === 'width' ? targetWidthStr : displayHeightStr}
                  onChange={(e) => {
                    if (primaryDim === 'width') {
                      handleWidthChange(e.target.value);
                    } else {
                      handleHeightChange(e.target.value);
                    }
                  }}
                  onBlur={(e) => {
                    const val = parseFloat(e.target.value);
                    if (!isNaN(val)) {
                      if (primaryDim === 'width') {
                        handleWidthChange(String(Math.floor(val)));
                      } else {
                        handleHeightChange(String(Math.floor(val)));
                      }
                    }
                  }}
                  className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50"
                />
                <button
                  type="button"
                  data-testid="primary-dim-toggle"
                  disabled={isRunning}
                  onClick={() => setPrimaryDim(d => d === 'width' ? 'height' : 'width')}
                  className="bg-gray-700 hover:bg-gray-600 px-2 rounded border border-gray-600 text-xs transition-colors text-gray-300"
                  title="Switch primary dimension"
                >
                  ⇄
                </button>
              </div>
            </div>
            
            <div className="text-gray-500 text-lg px-1 pt-4">×</div>
            
            <div className="flex-1 opacity-70">
              <label className="block text-xs text-gray-400 mb-1 capitalize">
                {primaryDim === 'width' ? 'height' : 'width'} (Computed)
              </label>
              <div
                data-testid={primaryDim === 'width' ? 'computed-target-height' : 'computed-target-width'}
                className="w-full bg-gray-900 border border-gray-800 rounded px-3 py-1.5 text-sm text-gray-500 cursor-not-allowed select-none"
              >
                {primaryDim === 'width' ? (targetHeight || '') : (targetWidth || '')}
              </div>
            </div>
          </div>

          <div className="flex items-center space-x-2 pt-1">
            <label className="text-sm text-gray-300">Scale Factor:</label>
            <input
              data-testid="resolution-scale-input"
              type="number"
              min={0.5} max={8} step={0.01}
              disabled={isRunning}
              value={scaleStr}
              onChange={(e) => handleScaleChange(e.target.value)}
              className="w-24 bg-gray-700 border border-gray-600 rounded px-2 py-1 text-sm focus:ring-2 focus:ring-blue-500 disabled:opacity-50"
            />
            <span className="text-xs text-gray-500">× source</span>
          </div>
        </>
      )}
    </div>
  );
}

