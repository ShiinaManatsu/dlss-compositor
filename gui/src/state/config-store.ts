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
  | { type: 'SET_PRESET'; payload: DlssConfig['preset'] }
  | { type: 'SET_INTERPOLATION'; payload: 0 | 2 | 4 }
  | { type: 'SET_CAMERA_DATA_FILE'; payload: string }
  | { type: 'SET_ENCODE_VIDEO'; payload: boolean }
  | { type: 'SET_VIDEO_OUTPUT_FILE'; payload: string }
  | { type: 'SET_FPS'; payload: number }
  | { type: 'SET_EXR_COMPRESSION'; payload: DlssConfig['exrCompression'] }
  | { type: 'SET_EXR_DWA_QUALITY'; payload: number }
  | { type: 'SET_OUTPUT_PASSES'; payload: ('beauty' | 'depth' | 'normals')[] }
  | { type: 'SET_TONEMAP_MODE'; payload: DlssConfig['tonemapMode'] }
  | { type: 'SET_INVERSE_TONEMAP'; payload: boolean }
  | { type: 'SET_FORWARD_LUT'; payload: string }
  | { type: 'SET_INVERSE_LUT'; payload: string }
  | { type: 'SET_MEMORY_BUDGET'; payload: number }
  | { type: 'SET_CHANNEL_MAP_FILE'; payload: string }
  | { type: 'SET_SOURCE_FPS'; payload: number }
  | { type: 'SET_SHARPNESS'; payload: number }
  | { type: 'SET_NGX_LOG_LEVEL'; payload: DlssConfig['ngxLogLevel'] }
  | { type: 'SET_SOURCE_RESOLUTION'; payload: { width: number | null; height: number | null } }
  | { type: 'CLEAR_SOURCE_RESOLUTION' }
  | { type: 'SET_TARGET_PRESET'; payload: '4K' | '1440p' | '1080p' | 'Custom' | 'DLAA' }
  | { type: 'SET_CUSTOM_TARGET_RESOLUTION'; payload: { width: number | null; height: number | null } }
  | { type: 'SET_PRESET_DLAA'; payload: DlssConfig['presetDLAA'] }
  | { type: 'SET_PRESET_QUALITY'; payload: DlssConfig['presetQuality'] }
  | { type: 'SET_PRESET_BALANCED'; payload: DlssConfig['presetBalanced'] }
  | { type: 'SET_PRESET_PERFORMANCE'; payload: DlssConfig['presetPerformance'] }
  | { type: 'SET_PRESET_ULTRA_PERFORMANCE'; payload: DlssConfig['presetUltraPerformance'] }
  | { type: 'SET_PRESET_ULTRA_QUALITY'; payload: DlssConfig['presetUltraQuality'] }
  | { type: 'SET_FEATURE_FLAG_HDR'; payload: boolean }
  | { type: 'SET_FEATURE_FLAG_MV_LOW_RES'; payload: boolean }
  | { type: 'SET_FEATURE_FLAG_AUTO_EXPOSURE'; payload: boolean }
  | { type: 'SET_ENABLE_OUTPUT_SUBRECTS'; payload: boolean }
  | { type: 'SET_EXPOSURE_SCALE'; payload: number }
  | { type: 'SET_FRAME_TIME_DELTA_MS'; payload: number }
  | { type: 'SET_DLSS_SHARPNESS'; payload: number }
  | { type: 'SET_CAMERA_CUT_DETECTION_ENABLED'; payload: boolean }
  | { type: 'SET_CAMERA_CUT_TRANSLATION_THRESHOLD'; payload: number }
  | { type: 'SET_CAMERA_CUT_ROTATION_THRESHOLD'; payload: number }
  | { type: 'SET_ENABLE_DYNAMIC_RESOLUTION'; payload: boolean };

interface ConfigContextValue {
  state: ConfigState;
  dispatch: React.Dispatch<ConfigAction>;
}

const ConfigContext = createContext<ConfigContextValue | undefined>(undefined);

const PRESET_WIDTHS: Record<string, number> = {
  '4K': 3840,
  '1440p': 2560,
  '1080p': 1920,
};

function computeScale(sourceWidth: number | null, targetPreset: string, customTargetWidth: number | null): number {
  if (targetPreset === 'DLAA') return 1.0;
  const targetW = targetPreset === 'Custom' ? customTargetWidth : (PRESET_WIDTHS[targetPreset] ?? null);
  if (targetW && sourceWidth && sourceWidth > 0) {
    return Math.round((targetW / sourceWidth) * 100) / 100; // Round to 2 decimal places
  }
  return 2.0; // fallback
}

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
    case 'SET_PRESET':
      return { ...state, preset: action.payload };
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
    case 'SET_EXR_COMPRESSION':
      return { ...state, exrCompression: action.payload };
    case 'SET_EXR_DWA_QUALITY':
      return { ...state, exrDwaQuality: action.payload };
    case 'SET_OUTPUT_PASSES':
      return { ...state, outputPasses: action.payload };
    case 'SET_TONEMAP_MODE':
      return { ...state, tonemapMode: action.payload };
    case 'SET_INVERSE_TONEMAP':
      return { ...state, inverseTonemapEnabled: action.payload };
    case 'SET_FORWARD_LUT':
      return { ...state, forwardLutFile: action.payload };
    case 'SET_INVERSE_LUT':
      return { ...state, inverseLutFile: action.payload };
    case 'SET_MEMORY_BUDGET':
      return { ...state, memoryBudgetGB: action.payload };
    case 'SET_CHANNEL_MAP_FILE':
      return { ...state, channelMapFile: action.payload };
    case 'SET_SOURCE_FPS':
      return { ...state, sourceFps: action.payload };
    case 'SET_SHARPNESS':
      return { ...state, sharpness: action.payload };
    case 'SET_NGX_LOG_LEVEL':
      return { ...state, ngxLogLevel: action.payload };
    case 'SET_SOURCE_RESOLUTION': {
      const { width, height } = action.payload;
      const newScale = computeScale(width, state.targetPreset, state.customTargetWidth);
      return { ...state, sourceWidth: width, sourceHeight: height, scaleFactor: newScale };
    }
    case 'CLEAR_SOURCE_RESOLUTION': {
      const newScale = computeScale(null, state.targetPreset, state.customTargetWidth);
      return { ...state, sourceWidth: null, sourceHeight: null, scaleFactor: newScale };
    }
    case 'SET_TARGET_PRESET': {
      const newPreset = action.payload;
      const newScale = computeScale(state.sourceWidth, newPreset, state.customTargetWidth);
      return { ...state, targetPreset: newPreset, scaleFactor: newScale };
    }
    case 'SET_CUSTOM_TARGET_RESOLUTION': {
      const { width, height } = action.payload;
      const newScale = computeScale(state.sourceWidth, 'Custom', width);
      return { ...state, customTargetWidth: width, customTargetHeight: height, scaleFactor: newScale };
    }
    case 'SET_PRESET_DLAA':
      return { ...state, presetDLAA: action.payload };
    case 'SET_PRESET_QUALITY':
      return { ...state, presetQuality: action.payload };
    case 'SET_PRESET_BALANCED':
      return { ...state, presetBalanced: action.payload };
    case 'SET_PRESET_PERFORMANCE':
      return { ...state, presetPerformance: action.payload };
    case 'SET_PRESET_ULTRA_PERFORMANCE':
      return { ...state, presetUltraPerformance: action.payload };
    case 'SET_PRESET_ULTRA_QUALITY':
      return { ...state, presetUltraQuality: action.payload };
    case 'SET_FEATURE_FLAG_HDR':
      return { ...state, featureFlagHDR: action.payload };
    case 'SET_FEATURE_FLAG_MV_LOW_RES':
      return { ...state, featureFlagMVLowRes: action.payload };
    case 'SET_FEATURE_FLAG_AUTO_EXPOSURE':
      return { ...state, featureFlagAutoExposure: action.payload };
    case 'SET_ENABLE_OUTPUT_SUBRECTS':
      return { ...state, enableOutputSubrects: action.payload };
    case 'SET_EXPOSURE_SCALE':
      return { ...state, exposureScale: action.payload };
    case 'SET_FRAME_TIME_DELTA_MS':
      return { ...state, frameTimeDeltaMsOverride: action.payload };
    case 'SET_DLSS_SHARPNESS':
      return { ...state, dlssSharpness: action.payload };
    case 'SET_CAMERA_CUT_DETECTION_ENABLED':
      return { ...state, cameraCutDetectionEnabled: action.payload };
    case 'SET_CAMERA_CUT_TRANSLATION_THRESHOLD':
      return { ...state, cameraCutTranslationThreshold: action.payload };
    case 'SET_CAMERA_CUT_ROTATION_THRESHOLD':
      return { ...state, cameraCutRotationThreshold: action.payload };
    case 'SET_ENABLE_DYNAMIC_RESOLUTION':
      return { ...state, enableDynamicResolution: action.payload };
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
