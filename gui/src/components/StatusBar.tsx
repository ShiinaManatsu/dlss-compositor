import React, { useEffect, useState } from 'react';
import { useBanner } from '../state/banner-store';

export default function StatusBar() {
  const { banner } = useBanner();
  const [exePath, setExePath] = useState<string | null>(null);
  const [exeChecked, setExeChecked] = useState(false);

  useEffect(() => {
    let cancelled = false;
    window.dlssApi.getResolvedExePath().then((resolved) => {
      if (cancelled) return;
      setExePath(resolved);
      setExeChecked(true);
    });
    return () => {
      cancelled = true;
    };
  }, []);

  if (exeChecked && !exePath) {
    return (
      <div data-testid="status-bar" className="bg-gray-900 border-b border-gray-700 px-4 py-1.5 flex items-center space-x-4 text-xs text-red-400">
        <span>dlss-compositor.exe not found — build the CLI or place it next to the GUI</span>
      </div>
    );
  }

  if (!banner) {
    return (
      <div data-testid="status-bar" className="bg-gray-900 border-b border-gray-700 px-4 py-1.5 flex items-center space-x-4 text-xs text-gray-400">
        <span>Detecting DLSS capabilities...</span>
      </div>
    );
  }

  return (
    <div data-testid="status-bar" className="bg-gray-900 border-b border-gray-700 px-4 py-1.5 flex items-center space-x-4 text-xs">
      <span className="text-gray-400">DLSS SDK {banner.sdk}</span>
      <span className="text-gray-600">|</span>
      <span className={banner.sr_available ? 'text-green-400' : 'text-red-400'}>
        DLSS-SR {banner.sr_available ? '✓' : '✗'}
      </span>
      <span className="text-gray-600">|</span>
      <span className={banner.fg_available ? 'text-green-400' : 'text-gray-500'}>
        DLSS-FG {banner.fg_available ? `✓ (${banner.max_multi_frame}x)` : '✗'}
      </span>
      <span className="text-gray-600">|</span>
      <span className="text-gray-400">Engine {banner.engine}</span>
    </div>
  );
}
