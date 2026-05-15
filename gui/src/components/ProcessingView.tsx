import React, { useState, useRef, useEffect, useCallback } from 'react';
import { useConfig } from '../state/config-store';
import { useProcessing } from '../hooks/useProcessing';

function formatElapsed(seconds: number): string {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
}

export default function ProcessingView() {
  const { state } = useConfig();
  const { status, currentFrame, totalFrames, errors, latestFramePath, start, stop } = useProcessing();
  const [exeAvailable, setExeAvailable] = useState(false);
  const [elapsed, setElapsed] = useState(0);
  const [showConfirm, setShowConfirm] = useState(false);
  const [completionMsg, setCompletionMsg] = useState('');
  const [previewPng, setPreviewPng] = useState<string | null>(null);
  const [previewError, setPreviewError] = useState<
    'ffmpeg-not-found' | 'exr-decode-error' | 'timeout' | 'unknown' | null
  >(null);
  const [ffmpegAvailable, setFfmpegAvailable] = useState<boolean | null>(null);
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const startTimeRef = useRef(0);

  useEffect(() => {
    let cancelled = false;
    window.dlssApi.getResolvedExePath().then((resolved) => {
      if (cancelled) return;
      setExeAvailable(!!resolved);
    });
    return () => {
      cancelled = true;
    };
  }, []);

  // Subscribe to the startup FFmpeg-availability ping from main so we can
  // show a "FFmpeg not installed" hint immediately, before any frame lands.
  useEffect(() => {
    const unlisten = window.dlssApi.onFFmpegStatus(({ available }) => {
      setFfmpegAvailable(available);
      if (!available) {
        setPreviewError('ffmpeg-not-found');
      }
    });
    return unlisten;
  }, []);

  useEffect(() => {
    if (status === 'running') {
      startTimeRef.current = Date.now();
      setElapsed(0);
      timerRef.current = setInterval(() => {
        setElapsed(Math.floor((Date.now() - startTimeRef.current) / 1000));
      }, 1000);
    } else {
      if (timerRef.current) {
        clearInterval(timerRef.current);
        timerRef.current = null;
      }
    }
    return () => {
      if (timerRef.current) {
        clearInterval(timerRef.current);
        timerRef.current = null;
      }
    };
  }, [status]);

  useEffect(() => {
    if (status === 'done' && elapsed > 0) {
      setCompletionMsg(`Processing complete — ${totalFrames} frames in ${formatElapsed(elapsed)}`);
    }
  }, [status, elapsed, totalFrames]);

  useEffect(() => {
    if (!latestFramePath) return;

    let isCancelled = false;
    window.dlssApi.getFramePreview(latestFramePath).then((result) => {
      if (isCancelled) return;
      if (result && result.success && result.pngPath) {
        setPreviewPng(result.pngPath);
        setPreviewError(null);
      } else {
        setPreviewPng(null);
        const reason = result?.reason as
          | 'ffmpeg-not-found'
          | 'exr-decode-error'
          | 'timeout'
          | 'unknown'
          | undefined;
        setPreviewError(reason ?? 'unknown');
      }
    });

    return () => {
      isCancelled = true;
    };
  }, [latestFramePath]);

  const isCustomTonemapMissingLut = state.tonemapMode === 'custom' && !state.forwardLutFile;

  const canStart =
    exeAvailable &&
    !!state.inputDir &&
    !!state.outputDir &&
    status !== 'running' &&
    (state.scaleEnabled || state.interpolateFactor > 0) &&
    (state.interpolateFactor === 0 || !!state.cameraDataFile) &&
    !isCustomTonemapMissingLut;

  const handleStart = useCallback(() => {
    setCompletionMsg('');
    start(state);
  }, [start, state]);

  const handleStopClick = () => {
    setShowConfirm(true);
  };

  const handleConfirmStop = () => {
    setShowConfirm(false);
    stop();
  };

  const handleCancelStop = () => {
    setShowConfirm(false);
  };

  useEffect(() => {
    if (!showConfirm) return;
    const handler = (e: KeyboardEvent) => { if (e.key === 'Escape') setShowConfirm(false); };
    document.addEventListener('keydown', handler);
    return () => document.removeEventListener('keydown', handler);
  }, [showConfirm]);

  const statusColor =
    status === 'running'
      ? 'bg-blue-500'
      : status === 'done'
        ? 'bg-green-500'
        : status === 'error'
          ? 'bg-red-500'
          : 'bg-gray-500';

  const statusLabel =
    status === 'running'
      ? 'Running'
      : status === 'done'
        ? 'Done'
        : status === 'error'
          ? 'Error'
          : 'Idle';

  const frameCounterLabel =
    currentFrame === 0 && totalFrames === 0 ? 'Starting…' : `${currentFrame}/${totalFrames}`;

  return (
    <div className="flex flex-col h-full p-4 gap-4">
      {/* Top controls */}
      <div className="flex items-center gap-3 flex-shrink-0 flex-wrap">
        <button
          data-testid="start-btn"
          disabled={!canStart}
          onClick={handleStart}
          className="px-6 py-2 bg-blue-600 hover:bg-blue-500 disabled:bg-gray-700 disabled:text-gray-500 disabled:cursor-not-allowed rounded font-medium text-sm transition-colors flex-shrink-0"
        >
          Start Processing
        </button>

        {status === 'running' && (
          <button
            data-testid="stop-btn"
            onClick={handleStopClick}
            className="px-4 py-2 bg-red-700 hover:bg-red-600 rounded font-medium text-sm transition-colors flex-shrink-0"
          >
            Stop
          </button>
        )}

        <div className="flex items-center gap-2 flex-1 min-w-0">
          {isCustomTonemapMissingLut && (
            <span className="text-yellow-500 text-xs truncate" title="Custom tonemap requires a forward LUT file">
              ⚠ Custom tonemap requires a forward LUT file
            </span>
          )}

          {status === 'error' && errors.length > 0 && (
            <div
              data-testid="error-banner"
              className="text-red-400 text-sm truncate bg-red-900/30 px-2 py-1 rounded border border-red-800/50"
              title={errors[errors.length - 1]}
            >
              ⚠ {errors[errors.length - 1]}
            </div>
          )}
        </div>

        <div className="flex items-center gap-2 ml-auto flex-shrink-0">
          <div
            data-testid="status-indicator"
            data-status={status}
            className={`w-2.5 h-2.5 rounded-full ${statusColor} ${status === 'running' ? 'animate-pulse' : ''}`}
          />
          <span className="text-sm text-gray-400">{statusLabel}</span>
          {status === 'running' && (
            <span data-testid="elapsed-time" className="text-sm text-gray-400 ml-2 font-mono">
              {formatElapsed(elapsed)}
            </span>
          )}
        </div>
      </div>

      {/* Completion banner */}
      {completionMsg && status === 'done' && (
        <div className="flex-shrink-0 px-3 py-2 bg-green-900/30 border border-green-600 text-green-400 rounded text-sm">
          {completionMsg}
        </div>
      )}

      {/* Frame preview */}
      <div
        data-testid="frame-preview"
        className="flex-1 bg-gray-950 border border-gray-700 rounded overflow-hidden min-h-0 flex items-center justify-center p-4"
      >
        {status === 'idle' && (
          <div className="text-gray-600 text-sm text-center">Ready to process. Configure settings and click Start.</div>
        )}
        {(status === 'running' || status === 'done') && (
          <div className="w-full h-full flex flex-col items-center justify-center">
            {previewPng ? (
              <div className="flex flex-col items-center justify-center h-full max-h-full">
                <img 
                  data-testid="frame-preview-img"
                  src={`file://${previewPng}`} 
                  className="max-w-full max-h-[85%] object-contain rounded shadow-lg" 
                  alt={`Frame ${currentFrame}`}
                  onError={() => setPreviewPng(null)}
                />
                <div className="text-xs text-gray-500 mt-3 font-mono text-center">
                  Frame {currentFrame}/{totalFrames}
                </div>
                <div className="text-xs text-gray-600 font-mono px-4 truncate max-w-sm mt-1" title={latestFramePath ?? undefined}>
                  {latestFramePath ? latestFramePath.split(/[\\/]/).pop() : ''}
                </div>
              </div>
            ) : (
              <div className="flex flex-col items-center justify-center w-full max-w-lg">
                <div data-testid="frame-preview-placeholder" className="text-gray-500 text-sm text-center py-16 w-full border border-dashed border-gray-700 bg-gray-900/50 rounded flex flex-col items-center justify-center">
                  <div className="mb-2">{status === 'running' && !latestFramePath ? 'Waiting for first frame...' : 'No preview available'}</div>
                  {previewError === 'ffmpeg-not-found' || ffmpegAvailable === false ? (
                    <div data-testid="preview-error-ffmpeg" className="text-xs text-yellow-400 text-center mt-2 px-3">
                      FFmpeg not found in PATH. Install from ffmpeg.org for live preview.
                    </div>
                  ) : previewError === 'exr-decode-error' ? (
                    <div data-testid="preview-error-decode" className="text-xs text-orange-400 text-center mt-2 px-3">
                      Frame format not supported by FFmpeg. Preview unavailable.
                    </div>
                  ) : previewError === 'timeout' ? (
                    <div data-testid="preview-error-timeout" className="text-xs text-red-400 text-center mt-2 px-3">
                      Preview generation timed out. Frame may be very large.
                    </div>
                  ) : previewError === 'unknown' ? (
                    <div data-testid="preview-error-unknown" className="text-xs text-red-400 text-center mt-2 px-3">
                      Preview generation failed.
                    </div>
                  ) : (
                    <div className="text-xs text-gray-600">Live preview via FFmpeg</div>
                  )}
                </div>
                <div className="text-5xl font-mono font-bold text-blue-400 mt-6 mb-3">
                  {frameCounterLabel.includes('/') ? (
                    <>
                      {currentFrame}<span className="text-gray-600 text-3xl">/{totalFrames}</span>
                    </>
                  ) : (
                    frameCounterLabel
                  )}
                </div>
                <div className="text-xs text-gray-500 font-mono px-4 truncate max-w-sm" title={latestFramePath ?? undefined}>
                  {latestFramePath ? latestFramePath.split(/[\\/]/).pop() : 'Waiting for first frame…'}
                </div>
              </div>
            )}
          </div>
        )}
        {status === 'error' && (
          <div className="text-red-500 text-sm">Processing failed</div>
        )}
      </div>

      {/* Stop confirmation dialog */}
      {showConfirm && (
        <div className="fixed inset-0 bg-black/50 flex items-center justify-center z-50">
          <div className="bg-gray-800 border border-gray-600 rounded-lg p-6 max-w-sm mx-4 shadow-xl">
            <p className="text-sm mb-4">Stop processing? Current frame will be lost.</p>
            <div className="flex justify-end gap-3">
              <button
                data-testid="stop-cancel"
                onClick={handleCancelStop}
                className="px-4 py-1.5 bg-gray-700 hover:bg-gray-600 rounded text-sm transition-colors"
              >
                Cancel
              </button>
              <button
                data-testid="stop-confirm"
                onClick={handleConfirmStop}
                className="px-4 py-1.5 bg-red-700 hover:bg-red-600 rounded text-sm transition-colors"
              >
                Confirm
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
