import { useState, useEffect } from 'react';

let isRunning = false;
const listeners = new Set<(running: boolean) => void>();

export const setGlobalIsRunning = (running: boolean) => {
  if (isRunning !== running) {
    isRunning = running;
    listeners.forEach(l => l(running));
  }
};

export const useIsRunning = () => {
  const [val, setVal] = useState(isRunning);
  useEffect(() => {
    listeners.add(setVal);
    return () => { listeners.delete(setVal); };
  }, []);
  return val;
};