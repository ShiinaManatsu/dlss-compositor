import 'dart:async';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../state/app_state.dart';
import '../plugin/dlss_compositor.dart';

/// Log entry with level and message.
class LogEntry {
  final String level;  // error, warn, info, debug
  final String message;
  final DateTime timestamp;
  LogEntry(this.level, this.message) : timestamp = DateTime.now();
}

/// Right-side detail panel with 4 sections:
/// 1. Live preview frame (2-4 fps from EventChannel + FFI)
/// 2. Progress bar + frame counter + ETA
/// 3. Log panel (rolling 200 entries)
/// 4. Completion statistics (shown when Done)
class DetailPanel extends StatefulWidget {
  const DetailPanel({super.key});

  @override
  State<DetailPanel> createState() => _DetailPanelState();
}

class _DetailPanelState extends State<DetailPanel> {
  Uint8List? _previewData;
  final List<LogEntry> _logs = [];
  static const int _maxLogEntries = 200;

  StreamSubscription? _progressSub;
  Timer? _previewTimer;

  // Completion stats
  DateTime? _startTime;
  DateTime? _endTime;
  int _outputFrameCount = 0;

  @override
  void initState() {
    super.initState();
    _startListening();
  }

  @override
  void dispose() {
    _progressSub?.cancel();
    _previewTimer?.cancel();
    super.dispose();
  }

  void _startListening() {
    // Subscribe to EventChannel progress stream
    _progressSub = DlssCompositor.progressStream.listen(
      (event) {
        if (!mounted) return;
        final type = event['type'] as String? ?? '';

        if (type == 'progress') {
          final frame = event['current_frame'] as int? ?? 0;
          final total = event['total_frames'] as int? ?? 0;
          final percent = total > 0 ? frame / total : 0.0;
          Provider.of<AppState>(context, listen: false)
              .updateProgress(frame, total, percent);

          if (total > 0 && frame == 0) {
            _startTime = DateTime.now();
          }
          _outputFrameCount = frame;

        } else if (type == 'log') {
          _addLog(event['level'] as String? ?? 'info',
                  event['message'] as String? ?? '');

        } else if (type == 'done') {
          _endTime = DateTime.now();
          Provider.of<AppState>(context, listen: false)
              .setTaskState(TaskState.done);
          setState(() {});
        }
      },
      onError: (e) => _addLog('error', 'Stream error: $e'),
    );

    // Poll preview frames at ~3 fps (throttled)
    _previewTimer = Timer.periodic(const Duration(milliseconds: 333), (_) async {
      if (!mounted) return;
      final appState = Provider.of<AppState>(context, listen: false);
      if (appState.taskState != TaskState.running) return;

      try {
        final frame = await DlssCompositor.getPreviewFrame();
        if (frame != null && mounted) {
          setState(() => _previewData = frame);
        }
      } catch (_) {}
    });
  }

  void _addLog(String level, String message) {
    if (!mounted) return;
    setState(() {
      _logs.add(LogEntry(level, message));
      // Rolling buffer: keep only last 200 entries
      if (_logs.length > _maxLogEntries) {
        _logs.removeAt(0);
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<AppState>(
      builder: (context, state, _) {
        return Column(
          children: [
            // 1. Preview frame
            Expanded(
              flex: 4,
              child: _buildPreviewSection(),
            ),

            // 2. Progress bar
            _buildProgressSection(state),

            // 3. Log panel
            Expanded(
              flex: 3,
              child: _buildLogSection(),
            ),

            // 4. Completion stats (only when Done)
            if (state.taskState == TaskState.done)
              _buildCompletionStats(state),
          ],
        );
      },
    );
  }

  Widget _buildPreviewSection() {
    return Container(
      color: Colors.black87,
      child: _previewData != null
          ? Image.memory(
              _previewData!,
              fit: BoxFit.contain,
              gaplessPlayback: true,  // Prevent flicker during updates
            )
          : const Center(
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Icon(Icons.image_not_supported, color: Colors.white38, size: 48),
                  SizedBox(height: 8),
                  Text('Preview frames will appear here during processing',
                      style: TextStyle(color: Colors.white38, fontSize: 12)),
                ],
              ),
            ),
    );
  }

  Widget _buildProgressSection(AppState state) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
      color: const Color(0xFF2D2D2D),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Text(
                state.taskState == TaskState.running
                    ? 'Processing: ${state.currentFrame}/${state.totalFrames}'
                    : state.taskState == TaskState.done
                        ? 'Complete ✓'
                        : 'Ready',
                style: const TextStyle(color: Colors.white70, fontSize: 12),
              ),
              const Spacer(),
              if (state.taskState == TaskState.running)
                Text(
                  '${(state.progressPercent * 100).toStringAsFixed(1)}%',
                  style: const TextStyle(color: Colors.green, fontSize: 12),
                ),
            ],
          ),
          if (state.taskState == TaskState.running || state.taskState == TaskState.done) ...[
            const SizedBox(height: 4),
            LinearProgressIndicator(
              value: state.progressPercent,
              backgroundColor: Colors.white12,
              valueColor: AlwaysStoppedAnimation<Color>(
                state.taskState == TaskState.done ? Colors.green : Colors.blue,
              ),
            ),
          ],
        ],
      ),
    );
  }

  Widget _buildLogSection() {
    return Container(
      color: const Color(0xFF1A1A1A),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Padding(
            padding: EdgeInsets.symmetric(horizontal: 8, vertical: 4),
            child: Text('Log', style: TextStyle(color: Colors.white54, fontSize: 11)),
          ),
          Expanded(
            child: ListView.builder(
              itemCount: _logs.length,
              itemBuilder: (ctx, i) {
                final log = _logs[i];
                Color color = switch (log.level) {
                  'error' => Colors.red,
                  'warn' || 'warning' => Colors.orange,
                  'debug' => Colors.grey,
                  _ => Colors.white70,
                };
                return Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 1),
                  child: Text(
                    '[${log.level.toUpperCase()}] ${log.message}',
                    style: TextStyle(color: color, fontSize: 11, fontFamily: 'monospace'),
                  ),
                );
              },
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildCompletionStats(AppState state) {
    final duration = (_startTime != null && _endTime != null)
        ? _endTime!.difference(_startTime!)
        : Duration.zero;

    return Container(
      padding: const EdgeInsets.all(12),
      color: const Color(0xFF1E3A1E),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text('Completed', style: TextStyle(color: Colors.green, fontWeight: FontWeight.bold)),
          const SizedBox(height: 4),
          Text('Time: ${duration.inSeconds}s | Frames: $_outputFrameCount',
              style: const TextStyle(color: Colors.white70, fontSize: 12)),
          const SizedBox(height: 4),
          ElevatedButton.icon(
            onPressed: () {
              // Open output folder in Explorer
              // final config = Provider.of<AppState>(context, listen: false).currentDlssConfig;
              // TODO: use output_dir from config
            },
            icon: const Icon(Icons.folder_open, size: 14),
            label: const Text('Open Output Folder'),
            style: ElevatedButton.styleFrom(
              padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
              textStyle: const TextStyle(fontSize: 12),
            ),
          ),
        ],
      ),
    );
  }
}
