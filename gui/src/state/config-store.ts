import React, { createContext, useContext, useReducer, ReactNode } from 'react';
import { DlssConfig, DEFAULT_CONFIG } from '../types/dlss-config';

type ConfigState = DlssConfig & {
  qualityManuallyOverridden: boolean;
};

type ConfigAction =
  | { type: 'SET_INPUT_DIR'; payload: string }
  | { type: 'SET_OUTPUT_DIR'; payload: string }
  | { type: 'SET_SCALE_ENABLED'; payload: boolean }
  | { type: 'SET_SCALE'; payload: number }
  | { type: 'SET_QUALITY'; payload: DlssConfig['quality'] }
  | { type: 'SET_INTERPOLATION'; payload: 0 | 2 | 4 }
  | { type: 'SET_CAMERA_DATA_FILE'; payload: string }
  | { type: 'SET_ENCODE_VIDEO'; payload: boolean }
  | { type: 'SET_VIDEO_OUTPUT_FILE'; payload: string }
  | { type: 'SET_FPS'; payload: number };

interface ConfigContextValue {
  state: ConfigState;
  dispatch: React.Dispatch<ConfigAction>;
}

const ConfigContext = createContext<ConfigContextValue | undefined>(undefined);

function configReducer(state: ConfigState, action: ConfigAction): ConfigState {
  switch (action.type) {
    case 'SET_INPUT_DIR':
      return { ...state, inputDir: action.payload };
    case 'SET_OUTPUT_DIR':
      return { ...state, outputDir: action.payload };
    case 'SET_SCALE_ENABLED':
      return { ...state, scaleEnabled: action.payload };
    case 'SET_SCALE': {
      const newScale = action.payload;
      let newQuality = state.quality;
      let newOverridden = state.qualityManuallyOverridden;

      if (!state.qualityManuallyOverridden) {
        if (newScale === 1.0) {
          newQuality = 'DLAA';
          newOverridden = false; // still auto
        } else if (newScale > 1.0) {
          newQuality = 'MaxQuality';
          newOverridden = false; // still auto
        }
      }

      return {
        ...state,
        scaleFactor: newScale,
        quality: newQuality,
        qualityManuallyOverridden: newOverridden,
      };
    }
    case 'SET_QUALITY':
      return { ...state, quality: action.payload, qualityManuallyOverridden: true };
    case 'SET_INTERPOLATION':
      return { ...state, interpolateFactor: action.payload };
    case 'SET_CAMERA_DATA_FILE':
      return { ...state, cameraDataFile: action.payload };
    case 'SET_ENCODE_VIDEO':
      return { ...state, encodeVideo: action.payload };
    case 'SET_VIDEO_OUTPUT_FILE':
      return { ...state, videoOutputFile: action.payload };
    case 'SET_FPS':
      return { ...state, fps: action.payload };
    default:
      return state;
  }
}

export function ConfigProvider({ children }: { children: ReactNode }) {
  const [state, dispatch] = useReducer(configReducer, {
    ...DEFAULT_CONFIG,
    qualityManuallyOverridden: false,
  });

  return React.createElement(ConfigContext.Provider, { value: { state, dispatch } }, children);
}

export function useConfig() {
  const context = useContext(ConfigContext);
  if (context === undefined) {
    throw new Error('useConfig must be used within a ConfigProvider');
  }
  return context;
}
