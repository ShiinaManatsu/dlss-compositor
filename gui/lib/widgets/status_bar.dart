import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../app_version.dart';
import '../l10n/app_strings.dart';
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
          height: 24,
          decoration: const BoxDecoration(
            color: Color(0xFF007ACC), // VS Code style remote blue/accent
            border: Border(top: BorderSide(color: Color(0xFF333333))),
          ),
          padding: const EdgeInsets.symmetric(horizontal: 12),
          child: Row(
            children: [
              // Left: version + GPU info
              if (state.dlssVersion.isNotEmpty) ...[
                const Icon(Icons.memory, size: 12, color: Colors.white),
                const SizedBox(width: 6),
                Text(
                  '${context.strings.connecting} $kAppVersion | DLSS ${state.dlssVersion} | ${state.gpuName} | ${context.strings.fgMaxLabel(state.fgMaxMultiplier)}',
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 11,
                  ),
                ),
              ] else
                Row(
                  children: [
                    const SizedBox(width: 10, height: 10, child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white)),
                    const SizedBox(width: 8),
                    Text(
                      '${context.strings.connecting} $kAppVersion - ${context.strings.isZh ? "连接中..." : "Connecting..."}',
                      style: const TextStyle(color: Colors.white, fontSize: 11),
                    ),
                  ],
                ),

              const Spacer(),

              // Right: Status
              Row(
                children: [
                  Icon(
                    state.taskState == TaskState.running
                        ? Icons.sync
                        : state.taskState == TaskState.cancelling
                            ? Icons.stop
                            : Icons.check,
                    size: 12,
                    color: Colors.white,
                  ),
                  const SizedBox(width: 4),
                  Text(
                    state.taskState == TaskState.running
                        ? '${state.currentFrame}/${state.totalFrames}'
                        : state.taskState == TaskState.cancelling
                            ? context.strings.cancelling
                            : context.strings.ready,
                    style: const TextStyle(color: Colors.white, fontSize: 11),
                  ),
                ],
              ),
            ],
          ),
        );
      },
    );
  }
}
