import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:dlss_compositor_plugin/dlss_compositor_plugin.dart';
import '../state/app_state.dart';
import '../plugin/dlss_compositor.dart';
import '../util/error_dialog.dart';

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
  ui.Image? _previewImage;
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
    _previewImage?.dispose();
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

          if (total > 0 && frame == 0 && _startTime == null) {
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
          
        } else if (type == 'error') {
          _endTime = DateTime.now();
          final msg = event['message'] as String? ?? 'Unknown error';
          Provider.of<AppState>(context, listen: false)
              .setTaskState(TaskState.error, error: msg);
          _addLog('error', msg);
          showErrorDialog(
            context,
            title: 'Backend error',
            message: 'The backend reported a runtime error.',
            details: msg,
          );
          setState(() {});
        }
      },
      onError: (e) {
        _addLog('error', 'Stream error: $e');
        if (mounted) {
          showErrorDialog(
            context,
            title: 'Backend stream error',
            message: 'The backend event stream failed.',
            details: '$e',
          );
        }
      },
    );

    // Poll preview frames at ~3 fps (throttled)
    _previewTimer = Timer.periodic(const Duration(milliseconds: 333), (_) async {
      if (!mounted) return;
      final appState = Provider.of<AppState>(context, listen: false);
      if (appState.taskState != TaskState.running) return;

      try {
        final frame = await DlssCompositor.getPreviewFrame();
        if (frame != null && mounted) {
          final image = await _decodePreviewFrame(frame);
          if (mounted) {
            setState(() {
              _previewImage?.dispose();
              _previewImage = image;
            });
          } else {
            image.dispose();
          }
        }
      } catch (_) {}
    });
  }

  Future<ui.Image> _decodePreviewFrame(PreviewFrame frame) async {
    final expected = frame.width * frame.height * 4;
    final bytes = frame.data.length >= expected
        ? Uint8List.sublistView(frame.data, 0, expected)
        : (Uint8List(expected)..setRange(0, frame.data.length, frame.data));
    final completer = Completer<ui.Image>();
    ui.decodeImageFromPixels(
      bytes,
      frame.width,
      frame.height,
      ui.PixelFormat.rgba8888,
      completer.complete,
    );
    return completer.future;
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
        // Reset local state if AppState went from non-running to running (new task)
        if (state.taskState == TaskState.running && _endTime != null) {
          WidgetsBinding.instance.addPostFrameCallback((_) {
            if (mounted) {
              setState(() {
                _startTime = null;
                _endTime = null;
                _outputFrameCount = 0;
                _logs.clear();
                _previewImage?.dispose();
                _previewImage = null;
              });
            }
          });
        }

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

            // 4. Completion stats
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
      child: _previewImage != null
          ? RawImage(
              image: _previewImage,
              fit: BoxFit.contain,
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
    String statusText = 'Ready';
    if (state.taskState == TaskState.running) {
      statusText = 'Processing: ${state.currentFrame}/${state.totalFrames}';
      if (_startTime != null && state.currentFrame > 0 && state.totalFrames > state.currentFrame) {
        final elapsed = DateTime.now().difference(_startTime!);
        final timePerFrame = elapsed.inMilliseconds / state.currentFrame;
        final remainingFrames = state.totalFrames - state.currentFrame;
        final etaMillis = timePerFrame * remainingFrames;
        final eta = Duration(milliseconds: etaMillis.round());
        statusText += '  (ETA: ${eta.inMinutes}:${(eta.inSeconds % 60).toString().padLeft(2, '0')})';
      }
    } else if (state.taskState == TaskState.done) {
      statusText = 'Complete ✓';
    } else if (state.taskState == TaskState.error) {
      statusText = 'Stopped';
    }

    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
      color: const Color(0xFF2D2D2D),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Text(
                statusText,
                style: TextStyle(
                  color: Colors.white70,
                  fontSize: 12
                ),
              ),
              const Spacer(),
              if (state.taskState == TaskState.running)
                Text(
                  '${(state.progressPercent * 100).toStringAsFixed(1)}%',
                  style: const TextStyle(color: Colors.green, fontSize: 12),
                ),
            ],
          ),
          if (state.taskState == TaskState.running || state.taskState == TaskState.done || state.taskState == TaskState.error) ...[
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
              final outputDir = state.outputDir;
              if (outputDir.isNotEmpty) {
                Process.start('explorer.exe', [outputDir]);
              }
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
