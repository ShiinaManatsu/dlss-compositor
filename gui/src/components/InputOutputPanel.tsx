import React, { useState } from 'react';
import { useConfig } from '../state/config-store';
import { useIsRunning } from '../state/processing-store';

export default function InputOutputPanel() {
  const { state, dispatch } = useConfig();
  const isRunning = useIsRunning();
  const [inputTouched, setInputTouched] = useState(false);
  const [outputTouched, setOutputTouched] = useState(false);
  const [autoDetectedCamera, setAutoDetectedCamera] = useState(false);
  const [exrCount, setExrCount] = useState<number | null>(null);

  const handleInputBrowse = async () => {
    try {
      const dir = await window.dlssApi.selectDirectory();
      if (dir) {
        dispatch({ type: 'SET_INPUT_DIR', payload: dir });
        setExrCount(null);
        
        try {
          const count = await window.dlssApi.countExrFiles(dir);
          setExrCount(count);
        } catch (err) {
          console.error('EXR file count failed:', err);
        }

        // Auto-detect camera.json
        try {
          const result = await window.dlssApi.cameraAutoDetect(dir);
          if (result.cameraJsonPath) {
            dispatch({ type: 'SET_CAMERA_DATA_FILE', payload: result.cameraJsonPath });
            setAutoDetectedCamera(true);
          } else {
            setAutoDetectedCamera(false);
          }
          if (result.sourceWidth !== null && result.sourceHeight !== null) {
            dispatch({ type: 'SET_SOURCE_RESOLUTION', payload: { width: result.sourceWidth, height: result.sourceHeight } });
          }
        } catch (err) {
          console.error('Camera auto-detect failed:', err);
        }
      }
    } catch (err) {
      console.error(err);
    }
  };

  const handleOutputBrowse = async () => {
    try {
      const dir = await window.dlssApi.selectDirectory();
      if (dir) {
        dispatch({ type: 'SET_OUTPUT_DIR', payload: dir });
      }
    } catch (err) {
      console.error(err);
    }
  };

  const isInputError = inputTouched && state.inputDir === '';
  const isOutputError = outputTouched && state.outputDir === '';

  return (
    <div className="p-4 space-y-4">
      <div>
        <label className="block text-sm font-medium mb-1">Input Directory</label>
        <div className="flex space-x-2">
          <input
            data-testid="input-dir-field"
            type="text"
            readOnly
            disabled={isRunning}
            value={state.inputDir}
            onBlur={() => setInputTouched(true)}
            className={`flex-1 bg-gray-700 border ${isInputError ? 'border-red-500' : 'border-gray-600'} rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50`}
          />
          <button
            data-testid="input-dir-browse"
            onClick={handleInputBrowse}
            disabled={isRunning}
            className="bg-gray-700 hover:bg-gray-600 px-3 py-1.5 rounded text-sm transition-colors border border-gray-600 disabled:opacity-50 disabled:cursor-not-allowed"
          >
            Browse
          </button>
        </div>
        {isInputError && (
          <span data-testid="input-dir-error" className="text-red-500 text-xs mt-1 block">
            Input directory is required
          </span>
        )}
        {exrCount !== null && (
          <span data-testid="exr-count-hint" className="text-xs text-gray-400 mt-1 block">
            {exrCount > 0 
              ? `✓ Found ${exrCount} EXR file${exrCount !== 1 ? 's' : ''}` 
              : '⚠ No EXR files found in this directory'}
          </span>
        )}
        {autoDetectedCamera && state.cameraDataFile && (
          <span data-testid="camera-auto-detected-hint" className="text-green-500 text-xs mt-1 block">
            ✓ Auto-detected camera.json from input directory
          </span>
        )}
      </div>

      <div>
        <label className="block text-sm font-medium mb-1">Output Directory</label>
        <div className="flex space-x-2">
          <input
            data-testid="output-dir-field"
            type="text"
            readOnly
            disabled={isRunning}
            value={state.outputDir}
            onBlur={() => setOutputTouched(true)}
            className={`flex-1 bg-gray-700 border ${isOutputError ? 'border-red-500' : 'border-gray-600'} rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none disabled:opacity-50`}
          />
          <button
            data-testid="output-dir-browse"
            onClick={handleOutputBrowse}
            disabled={isRunning}
            className="bg-gray-700 hover:bg-gray-600 px-3 py-1.5 rounded text-sm transition-colors border border-gray-600 disabled:opacity-50 disabled:cursor-not-allowed"
          >
            Browse
          </button>
        </div>
        {isOutputError && (
          <span data-testid="output-dir-error" className="text-red-500 text-xs mt-1 block">
            Output directory is required
          </span>
        )}
      </div>
    </div>
  );
}
