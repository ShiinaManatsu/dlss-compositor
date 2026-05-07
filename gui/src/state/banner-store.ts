import React, { createContext, useContext, useState, ReactNode } from 'react';

export interface BannerData {
  sdk: string;
  engine: string;
  sr_available: boolean;
  fg_available: boolean;
  max_multi_frame: number;
}

interface BannerContextValue {
  banner: BannerData | null;
  setBanner: (banner: BannerData) => void;
}

const BannerContext = createContext<BannerContextValue | undefined>(undefined);

export function BannerProvider({ children }: { children: ReactNode }) {
  const [banner, setBannerState] = useState<BannerData | null>(null);
  const setBanner = (b: BannerData) => setBannerState(b);
  
  return React.createElement(BannerContext.Provider, { value: { banner, setBanner } }, children);
}

export function useBanner() {
  const context = useContext(BannerContext);
  if (!context) throw new Error('useBanner must be used within BannerProvider');
  return context;
}
