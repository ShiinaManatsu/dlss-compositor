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
  const { status, progress, currentFrame, totalFrames, log, errors, start, stop } = useProcessing();
  const [exePath, setExePath] = useState('');
  const [elapsed, setElapsed] = useState(0);
  const [showConfirm, setShowConfirm] = useState(false);
  const [completionMsg, setCompletionMsg] = useState('');
  const logRef = useRef<HTMLDivElement>(null);
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const startTimeRef = useRef(0);

  useEffect(() => {
    window.dlssApi.getSettings().then((raw: unknown) => {
      const s = raw as { exePath?: string };
      if (s?.exePath) setExePath(s.exePath);
    });
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
    const el = logRef.current;
    if (el) {
      el.scrollTop = el.scrollHeight;
    }
  }, [log]);

  const canStart =
    !!exePath &&
    !!state.inputDir &&
    !!state.outputDir &&
    status !== 'running' &&
    (state.scaleEnabled || state.interpolateFactor > 0) &&
    (state.interpolateFactor === 0 || !!state.cameraDataFile);

  const handleStart = useCallback(() => {
    setCompletionMsg('');
    start(state, exePath);
  }, [start, state, exePath]);

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

  return (
    <div className="flex flex-col h-full p-4 gap-4">
      {/* Top controls */}
      <div className="flex items-center gap-3 flex-shrink-0">
        <button
          data-testid="start-btn"
          disabled={!canStart}
          onClick={handleStart}
          className="px-6 py-2 bg-blue-600 hover:bg-blue-500 disabled:bg-gray-700 disabled:text-gray-500 disabled:cursor-not-allowed rounded font-medium text-sm transition-colors"
        >
          Start Processing
        </button>

        {status === 'running' && (
          <button
            data-testid="stop-btn"
            onClick={handleStopClick}
            className="px-4 py-2 bg-red-700 hover:bg-red-600 rounded font-medium text-sm transition-colors"
          >
            Stop
          </button>
        )}

        <div className="flex items-center gap-2 ml-auto">
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

      {/* Progress bar */}
      {(status === 'running' || status === 'done') && totalFrames > 0 && (
        <div className="flex-shrink-0">
          <div className="flex items-center justify-between text-xs text-gray-400 mb-1">
            <span data-testid="progress-text">
              Processing frame {currentFrame}/{totalFrames} ({progress}%)
            </span>
          </div>
          <div className="w-full h-2 bg-gray-700 rounded overflow-hidden">
            <div
              data-testid="progress-bar"
              className={`h-full transition-all duration-300 ${status === 'done' ? 'bg-green-500' : 'bg-blue-500'}`}
              style={{ width: `${progress}%` }}
            />
          </div>
        </div>
      )}

      {/* Completion banner */}
      {completionMsg && status === 'done' && (
        <div className="flex-shrink-0 px-3 py-2 bg-green-900/30 border border-green-600 text-green-400 rounded text-sm">
          {completionMsg}
        </div>
      )}

      {/* Error banner */}
      {status === 'error' && errors.length > 0 && (
        <div
          data-testid="error-banner"
          className="flex-shrink-0 px-3 py-2 bg-red-900/30 border border-red-600 text-red-400 rounded text-sm flex items-start justify-between"
        >
          <span className="break-all">{errors[errors.length - 1]}</span>
          <button
            data-testid="error-dismiss"
            onClick={() => setCompletionMsg('')}
            className="ml-2 text-red-400 hover:text-red-300 flex-shrink-0"
          >
            ✕
          </button>
        </div>
      )}

      {/* Log output */}
      <div
        ref={logRef}
        data-testid="log-output"
        className="flex-1 bg-gray-950 border border-gray-700 rounded p-3 overflow-y-auto font-mono text-xs leading-relaxed min-h-0"
      >
        {log.length === 0 && status === 'idle' ? (
          <div className="flex items-center justify-center h-full text-gray-600">
            Ready to process. Configure settings and click Start.
          </div>
        ) : (
          log.map((line, i) => (
            <div
              key={i}
              className={errors.includes(line) ? 'text-red-400' : 'text-gray-300'}
            >
              {line}
            </div>
          ))
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
