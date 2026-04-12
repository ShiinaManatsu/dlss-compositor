import React from 'react';

interface LayoutProps {
  sidebar: React.ReactNode;
  main: React.ReactNode;
}

export default function Layout({ sidebar, main }: LayoutProps) {
  return (
    <div className="flex h-[calc(100vh-2rem)] overflow-hidden">
      <div 
        data-testid="settings-panel" 
        className="w-[380px] min-w-[380px] border-r border-gray-600 bg-gray-800 overflow-y-auto"
      >
        {sidebar}
      </div>
      <div data-testid="main-area" className="flex-1 bg-gray-900 overflow-hidden relative">
        {main}
      </div>
    </div>
  );
}
