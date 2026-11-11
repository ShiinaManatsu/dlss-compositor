import {useState} from 'react';
import {ActionButton} from '@react-spectrum/s2/ActionButton';
import {Tooltip, TooltipTrigger} from '@react-spectrum/s2/Tooltip';
import Lighten from '@react-spectrum/s2/icons/Lighten';
import Contrast from '@react-spectrum/s2/icons/Contrast';
import BrightnessContrast from '@react-spectrum/s2/icons/BrightnessContrast';

export type ThemePreference = 'light' | 'dark' | 'system';
const modes: ThemePreference[] = ['light', 'dark', 'system'];
export function useThemePreference(storageKey = 's2-theme') {
  const [preference, setPreference] = useState<ThemePreference>(() => {
    try {const saved = localStorage.getItem(storageKey); return modes.includes(saved as ThemePreference) ? saved as ThemePreference : 'system';}
    catch {return 'system';}
  });
  const change = (value: ThemePreference) => {
    setPreference(value);
    try {localStorage.setItem(storageKey, value);} catch {/* A blocked store must not disable theme switching. */}
  };
  return {preference, setPreference: change, colorScheme: preference === 'system' ? undefined : preference};
}

/** Current-mode icons are this app profile's convention. Localize labels at the caller. */
export function ThemeCycleButton({preference, onChange, isDisabled,
  names = {light: 'Light', dark: 'Dark', system: 'System'},
  formatLabel = (current, next) => `Theme: ${current}; switch to ${next}`,
  formatTooltip = (current, next) => `${current} · Switch to ${next}`}: {
  preference: ThemePreference; onChange: (value: ThemePreference) => void;
  isDisabled?: boolean;
  names?: Record<ThemePreference, string>;
  formatLabel?: (current: string, next: string) => string;
  formatTooltip?: (current: string, next: string) => string;
}) {
  const next = modes[(modes.indexOf(preference) + 1) % modes.length];
  const Icon = preference === 'light' ? Lighten : preference === 'dark' ? Contrast : BrightnessContrast;
  return <TooltipTrigger><ActionButton isQuiet isDisabled={isDisabled} aria-label={formatLabel(names[preference], names[next])}
    onPress={() => onChange(next)}><Icon /></ActionButton>
    <Tooltip>{formatTooltip(names[preference], names[next])}</Tooltip></TooltipTrigger>;
}
