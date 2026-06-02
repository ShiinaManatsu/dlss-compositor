import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../state/app_state.dart';

/// VS Code-style bottom status bar.
/// Left: DLSS version + GPU + FG max
/// Right: Progress bar (when running)
class DlssStatusBar extends StatelessWidget {
  const DlssStatusBar({super.key});

  @override
  Widget build(BuildContext context) {
    return Consumer<AppState>(
      builder: (context, state, _) {
        return Container(
          height: 28,
          color: const Color(0xFF1E1E1E),
          padding: const EdgeInsets.symmetric(horizontal: 12),
          child: Row(
            children: [
              // Left: version + GPU info
              if (state.dlssVersion.isNotEmpty) ...[
                const Icon(Icons.memory, size: 14, color: Colors.green),
                const SizedBox(width: 4),
                Text(
                  'DLSS ${state.dlssVersion} | ${state.gpuName} | FG Max ${state.fgMaxMultiplier}x',
                  style: const TextStyle(
                    color: Colors.white70,
                    fontSize: 12,
                    fontFamily: 'monospace',
                  ),
                ),
              ] else
                const Text(
                  'DLSS Compositor v2 — Connecting...',
                  style: TextStyle(color: Colors.white54, fontSize: 12),
                ),

              const Spacer(),

              // Right: progress (shown when running)
              if (state.taskState == TaskState.running) ...[
                Text(
                  '${state.currentFrame}/${state.totalFrames}',
                  style: const TextStyle(color: Colors.white70, fontSize: 12),
                ),
                const SizedBox(width: 8),
                SizedBox(
                  width: 120,
                  child: LinearProgressIndicator(
                    value: state.totalFrames > 0
                        ? state.currentFrame / state.totalFrames
                        : 0,
                    backgroundColor: Colors.white24,
                    valueColor:
                        const AlwaysStoppedAnimation<Color>(Colors.green),
                  ),
                ),
              ],
            ],
          ),
        );
      },
    );
  }
}
