import React, { createContext, useCallback, useContext, useEffect, useState, ReactNode } from 'react';

export interface BannerData {
  event?: string;
  type?: string;
  sdk?: string;
  engine?: string;
  sr_available?: boolean;
  fg_available?: boolean;
  max_multi_frame?: number;
  ngx_sdk_version?: string;
  ngx_api_version_used?: string;
  ngx_api_version_path?: string;
  dlss_dll_version?: string;
  dlssg_dll_version?: string;
}

export type BannerStatus = 'detecting' | 'ready' | 'error';

interface BannerContextValue {
  banner: BannerData | null;
  status: BannerStatus;
  error: string | null;
  setBanner: (banner: BannerData) => void;
  setBannerError: (error: string) => void;
}

const BannerContext = createContext<BannerContextValue | undefined>(undefined);

const DLSS_DETECTION_ERROR = 'DLSS detection failed. Check driver and DLSS DLL installation.';

export function BannerProvider({ children }: { children: ReactNode }) {
  const [banner, setBannerState] = useState<BannerData | null>(null);
  const [status, setStatus] = useState<BannerStatus>('detecting');
  const [error, setError] = useState<string | null>(null);
  const setBanner = useCallback((b: BannerData) => {
    setBannerState(b);
    setStatus('ready');
    setError(null);
  }, []);
  const setBannerError = useCallback((message: string) => {
    setStatus('error');
    setError(message);
  }, []);

  useEffect(() => {
    if (typeof window === 'undefined' || !window.dlssApi) {
      return undefined;
    }

    const unlistenBanner = window.dlssApi.onBanner((data) => setBanner(data));
    const unlistenError = window.dlssApi.onError((message) => {
      if (message.startsWith('DLSS detection failed:')) {
        setBannerError(DLSS_DETECTION_ERROR);
      }
    });

    return () => {
      unlistenBanner();
      unlistenError();
    };
  }, [setBanner, setBannerError]);
  
  return React.createElement(BannerContext.Provider, { value: { banner, status, error, setBanner, setBannerError } }, children);
}

export function useBanner() {
  const context = useContext(BannerContext);
  if (!context) throw new Error('useBanner must be used within BannerProvider');
  return context;
}
