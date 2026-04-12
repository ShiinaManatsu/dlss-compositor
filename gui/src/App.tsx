import React, { useEffect } from 'react';
import { ConfigProvider, useConfig } from './state/config-store';
import Layout from './components/Layout';
import InputOutputPanel from './components/InputOutputPanel';
import BasicSettings from './components/BasicSettings';
import AdvancedSettings from './components/AdvancedSettings';
import ExePathConfig from './components/ExePathConfig';
import ProcessingView from './components/ProcessingView';
import type { Settings } from './types/dlss-config';

function AppContent() {
  const { dispatch } = useConfig();

  useEffect(() => {
    window.dlssApi.getSettings().then((raw: unknown) => {
      const settings = raw as Settings;
      if (!settings) return;
      // Load dirs from top-level Settings fields
      if (settings.lastInputDir) dispatch({ type: 'SET_INPUT_DIR', payload: settings.lastInputDir });
      if (settings.lastOutputDir) dispatch({ type: 'SET_OUTPUT_DIR', payload: settings.lastOutputDir });
      // Load config fields
      const c = settings.config;
      if (!c) return;
      if (c.scaleEnabled !== undefined) dispatch({ type: 'SET_SCALE_ENABLED', payload: c.scaleEnabled });
      if (c.scaleFactor !== undefined) dispatch({ type: 'SET_SCALE', payload: c.scaleFactor });
      if (c.quality !== undefined) dispatch({ type: 'SET_QUALITY', payload: c.quality });
      if (c.interpolateFactor !== undefined) dispatch({ type: 'SET_INTERPOLATION', payload: c.interpolateFactor });
      if (c.cameraDataFile !== undefined) dispatch({ type: 'SET_CAMERA_DATA_FILE', payload: c.cameraDataFile });
      if (c.encodeVideo !== undefined) dispatch({ type: 'SET_ENCODE_VIDEO', payload: c.encodeVideo });
      if (c.videoOutputFile !== undefined) dispatch({ type: 'SET_VIDEO_OUTPUT_FILE', payload: c.videoOutputFile });
      if (c.fps !== undefined) dispatch({ type: 'SET_FPS', payload: c.fps });
      if (c.exrCompression !== undefined) dispatch({ type: 'SET_EXR_COMPRESSION', payload: c.exrCompression });
      if (c.exrDwaQuality !== undefined) dispatch({ type: 'SET_EXR_DWA_QUALITY', payload: c.exrDwaQuality });
      if (c.memoryBudgetGB !== undefined) dispatch({ type: 'SET_MEMORY_BUDGET', payload: c.memoryBudgetGB });
      if (c.tonemapMode !== undefined) dispatch({ type: 'SET_TONEMAP_MODE', payload: c.tonemapMode });
      if (c.inverseTonemapEnabled !== undefined) dispatch({ type: 'SET_INVERSE_TONEMAP', payload: c.inverseTonemapEnabled });
      if (c.forwardLutFile !== undefined) dispatch({ type: 'SET_FORWARD_LUT', payload: c.forwardLutFile });
      if (c.inverseLutFile !== undefined) dispatch({ type: 'SET_INVERSE_LUT', payload: c.inverseLutFile });
      if (c.channelMapFile !== undefined) dispatch({ type: 'SET_CHANNEL_MAP_FILE', payload: c.channelMapFile });
    });
  }, [dispatch]);

  return (
    <div data-testid="app-root" className="h-screen bg-gray-900 text-gray-100 overflow-hidden flex flex-col">
      <div className="h-8 border-b border-gray-700 bg-gray-950 flex items-center px-4 flex-shrink-0">
        <h1 className="font-semibold">
          DLSS Compositor <span className="text-gray-400 text-sm ml-2 font-normal">v0.1.0</span>
        </h1>
      </div>
      
      <Layout
        sidebar={
          <div className="flex flex-col">
            <ExePathConfig />
            <InputOutputPanel />
            <BasicSettings />
            <AdvancedSettings />
          </div>
        }
        main={
          <ProcessingView />
        }
      />
    </div>
  );
}

function App() {
  return (
    <ConfigProvider>
      <AppContent />
    </ConfigProvider>
  );
}

export default App
