import React, { useState, useEffect } from 'react';

export default function ExePathConfig() {
  const [exePath, setExePath] = useState('');
  const [isValid, setIsValid] = useState<boolean | null>(null);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    window.dlssApi.getSettings().then((settings: unknown) => {
      const s = settings as { exePath?: string };
      if (s?.exePath) {
        setExePath(s.exePath);
        window.dlssApi.validateExePath(s.exePath).then(setIsValid);
      }
      setLoading(false);
    });
  }, []);

  const handleBrowse = async () => {
    try {
      const filePath = await window.dlssApi.selectFile([
        { name: 'Executable', extensions: ['exe'] },
      ]);
      if (!filePath) return;

      const valid = await window.dlssApi.validateExePath(filePath);
      setExePath(filePath);
      setIsValid(valid);

      if (valid) {
        await window.dlssApi.saveSettings({ exePath: filePath });
      }
    } catch (err) {
      console.error(err);
    }
  };

  if (loading) return null;

  const showWarning = !exePath;
  const showError = exePath && isValid === false;

  return (
    <div className="p-4 border-b border-gray-700">
      <label className="block text-sm font-medium mb-1">DLSS Compositor Executable</label>
      <div className="flex space-x-2">
        <input
          data-testid="exe-path-field"
          type="text"
          readOnly
          value={exePath}
          className={`flex-1 bg-gray-700 border ${showError ? 'border-red-500' : 'border-gray-600'} rounded px-3 py-1.5 text-sm focus:ring-2 focus:ring-blue-500 focus:outline-none truncate`}
        />
        <button
          data-testid="exe-path-browse"
          onClick={handleBrowse}
          className="bg-gray-700 hover:bg-gray-600 px-3 py-1.5 rounded text-sm transition-colors border border-gray-600"
        >
          Browse
        </button>
      </div>

      {showWarning && (
        <div
          data-testid="exe-path-warning"
          className="mt-2 px-3 py-2 text-xs bg-yellow-900/30 border border-yellow-600 text-yellow-400 rounded"
        >
          DLSS Compositor executable not configured
        </div>
      )}

      {showError && (
        <div
          data-testid="exe-path-error"
          className="mt-2 px-3 py-2 text-xs bg-red-900/30 border border-red-600 text-red-400 rounded"
        >
          Executable not found at path
        </div>
      )}
    </div>
  );
}
