import React from 'react';
import { ConfigProvider } from './state/config-store';
import Layout from './components/Layout';
import InputOutputPanel from './components/InputOutputPanel';
import BasicSettings from './components/BasicSettings';
import AdvancedSettings from './components/AdvancedSettings';

function App() {
  return (
    <ConfigProvider>
      <div data-testid="app-root" className="h-screen bg-gray-900 text-gray-100 overflow-hidden flex flex-col">
        <div className="h-8 border-b border-gray-700 bg-gray-950 flex items-center px-4 flex-shrink-0">
          <h1 className="font-semibold">
            DLSS Compositor <span className="text-gray-400 text-sm ml-2 font-normal">v0.1.0</span>
          </h1>
        </div>
        
        <Layout
          sidebar={
            <div className="flex flex-col">
              <InputOutputPanel />
              <BasicSettings />
              <AdvancedSettings />
            </div>
          }
          main={
            <div className="flex items-center justify-center h-full text-gray-500">
              Processing view placeholder
            </div>
          }
        />
      </div>
    </ConfigProvider>
  )
}

export default App
