import React, { useEffect, useState } from 'react';
import { useBanner } from '../state/banner-store';

export default function StatusBar() {
  const { banner, status, error, setBannerError } = useBanner();
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

  useEffect(() => {
    if (!exeChecked || !exePath || status !== 'detecting') return undefined;

    const timeout = window.setTimeout(() => {
      setBannerError('DLSS detection failed. Check driver and DLSS DLL installation.');
    }, 15_000);

    return () => window.clearTimeout(timeout);
  }, [exeChecked, exePath, status, setBannerError]);

  if (exeChecked && !exePath) {
    return (
      <div data-testid="status-bar" className="bg-gray-900 border-b border-gray-700 px-4 py-1.5 flex items-center space-x-4 text-xs text-red-400">
        <span>Configure exe path — dlss-compositor.exe not found</span>
      </div>
    );
  }

  if (status === 'error') {
    return (
      <div data-testid="status-bar" className="bg-gray-900 border-b border-gray-700 px-4 py-1.5 flex items-center space-x-4 text-xs text-red-400">
        <span title={error ?? undefined}>DLSS detection failed. Check driver and DLSS DLL installation.</span>
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

  const sdkVersion = banner.ngx_sdk_version ?? banner.sdk ?? 'unknown';
  const apiVersion = banner.ngx_api_version_used;
  const apiPath = banner.ngx_api_version_path;
  const dlssDllVersion = banner.dlss_dll_version;
  const dlssgDllVersion = banner.dlssg_dll_version;
  const hasLegacyAvailability = typeof banner.sr_available === 'boolean' || typeof banner.fg_available === 'boolean';

  return (
    <div data-testid="status-bar" className="bg-gray-900 border-b border-gray-700 px-4 py-1.5 flex items-center space-x-4 text-xs">
      <span className="text-gray-400">DLSS SDK {sdkVersion}</span>
      <span className="text-gray-600">|</span>
      {dlssDllVersion ? (
        <span className="text-green-400">DLSS DLL {dlssDllVersion}</span>
      ) : hasLegacyAvailability ? (
        <span className={banner.sr_available ? 'text-green-400' : 'text-red-400'}>
          DLSS-SR {banner.sr_available ? '✓' : '✗'}
        </span>
      ) : (
        <span className="text-green-400">DLSS detected ✓</span>
      )}
      {(dlssgDllVersion || hasLegacyAvailability) && <span className="text-gray-600">|</span>}
      {dlssgDllVersion ? (
        <span className="text-green-400">DLSSG DLL {dlssgDllVersion}</span>
      ) : hasLegacyAvailability ? (
        <span className={banner.fg_available ? 'text-green-400' : 'text-gray-500'}>
          DLSS-FG {banner.fg_available ? `✓ (${banner.max_multi_frame}x)` : '✗'}
        </span>
      ) : null}
      {apiVersion && <span className="text-gray-600">|</span>}
      {apiVersion && <span className="text-gray-400">NGX API {apiVersion}{apiPath ? ` (${apiPath})` : ''}</span>}
      {banner.engine && <span className="text-gray-600">|</span>}
      {banner.engine && <span className="text-gray-400">Engine {banner.engine}</span>}
    </div>
  );
}
