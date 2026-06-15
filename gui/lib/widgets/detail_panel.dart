import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:dlss_compositor_plugin/dlss_compositor_plugin.dart';
import '../l10n/app_strings.dart';
import '../state/app_state.dart';
import '../plugin/dlss_compositor.dart';
import '../util/error_dialog.dart';
import '../util/app_log.dart';

/// Log entry with level and message.
class LogEntry {
  final String level; // error, warn, info, debug
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
  bool _logsExpanded = false;

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
    AppLog.phase('detail', 'Subscribing to progress stream');
    _progressSub = DlssCompositor.progressStream.listen(
      (event) {
        if (!mounted) return;
        final type = event['type'] as String? ?? '';

        if (type == 'progress') {
          final frame = event['current_frame'] as int? ?? 0;
          final total = event['total_frames'] as int? ?? 0;
          final percent = total > 0 ? frame / total : 0.0;
          Provider.of<AppState>(
            context,
            listen: false,
          ).updateProgress(frame, total, percent);

          if (total > 0 && frame == 0 && _startTime == null) {
            _startTime = DateTime.now();
          }
          _outputFrameCount = frame;
          if (frame == 0 || frame == total || frame % 30 == 0) {
            AppLog.phase('detail', 'Progress event applied', {
              'frame': frame,
              'total': total,
              'percent': percent,
            });
          }
        } else if (type == 'log') {
          _addLog(
            event['level'] as String? ?? 'info',
            event['message'] as String? ?? '',
          );
        } else if (type == 'write_complete') {
          AppLog.phase('detail', 'Write complete event received');
          _applyProgressPayload(event);
        } else if (type == 'done') {
          AppLog.phase('detail', 'Done event received');
          _endTime = DateTime.now();
          _applyProgressPayload(event);
          Provider.of<AppState>(
            context,
            listen: false,
          ).setTaskState(TaskState.done);
          setState(() {});
        } else if (type == 'cancelled') {
          AppLog.phase('detail', 'Cancelled event received');
          _endTime = DateTime.now();
          _applyProgressPayload(event);
          Provider.of<AppState>(
            context,
            listen: false,
          ).setTaskState(TaskState.idle);
          _addLog('info', context.strings.taskCancelled);
          setState(() {});
        } else if (type == 'error') {
          _endTime = DateTime.now();
          _applyProgressPayload(event);
          final msg = event['message'] as String? ?? 'Unknown error';
          AppLog.phase('detail', 'Error event received', msg);
          Provider.of<AppState>(
            context,
            listen: false,
          ).setTaskState(TaskState.error, error: msg);
          _addLog('error', msg);
          showErrorDialog(
            context,
            title: context.strings.backendErrorTitle,
            message: context.strings.backendErrorMessage,
            details: msg,
          );
          setState(() {});
        }
      },
      onError: (e) {
        AppLog.phase('detail', 'Progress stream error', e);
        _addLog('error', 'Stream error: $e');
        if (mounted) {
          showErrorDialog(
            context,
            title: context.strings.backendStreamErrorTitle,
            message: context.strings.backendStreamErrorMessage,
            details: '$e',
          );
        }
      },
    );

    // Poll progress and preview frames at ~3 fps. Progress is read through
    // MethodChannel to avoid high-frequency EventChannel traffic from native.
    _previewTimer = Timer.periodic(const Duration(milliseconds: 333), (
      _,
    ) async {
      if (!mounted) return;
      final appState = Provider.of<AppState>(context, listen: false);
      if (appState.taskState != TaskState.running &&
          appState.taskState != TaskState.cancelling) {
        return;
      }

      try {
        final progress = await DlssCompositor.getProgress();
        final currentFrame =
            progress['current_frame'] as int? ?? appState.currentFrame;
        final total = progress['total_frames'] as int? ?? appState.totalFrames;
        final percent =
            progress['percent'] as double? ??
            (total > 0 ? currentFrame / total : 0.0);
        final dlssCurrentFrame =
            progress['dlss_current_frame'] as int? ?? appState.dlssCurrentFrame;
        final dlssTotal =
            progress['dlss_total_frames'] as int? ?? appState.dlssTotalFrames;
        final dlssPercent =
            progress['dlss_percent'] as double? ??
            (dlssTotal > 0 ? dlssCurrentFrame / dlssTotal : percent);
        appState.updateProgress(
          currentFrame,
          total,
          percent,
          dlssFrame: dlssCurrentFrame,
          dlssTotal: dlssTotal,
          dlssPercent: dlssPercent,
        );
        if (total > 0 && currentFrame == 0 && _startTime == null) {
          _startTime = DateTime.now();
        }
        _outputFrameCount = currentFrame;

        final previewFrame = await DlssCompositor.getPreviewFrame();
        if (previewFrame != null && mounted) {
          final image = await _decodePreviewFrame(previewFrame);
          if (mounted) {
            setState(() {
              _previewImage?.dispose();
              _previewImage = image;
            });
          } else {
            image.dispose();
          }
        }
      } catch (e) {
        AppLog.phase('preview', 'Preview poll failed', e);
      }
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

  void _applyProgressPayload(Map<String, dynamic> event) {
    final frame = event['current_frame'] as int?;
    final total = event['total_frames'] as int?;
    if (frame == null || total == null || total <= 0) {
      return;
    }
    final dlssFrame = event['dlss_current_frame'] as int?;
    final dlssTotal = event['dlss_total_frames'] as int?;
    final percent = frame / total;
    final dlssPercent = dlssTotal != null && dlssTotal > 0 && dlssFrame != null
        ? dlssFrame / dlssTotal
        : percent;
    Provider.of<AppState>(context, listen: false).updateProgress(
      frame,
      total,
      percent,
      dlssFrame: dlssFrame,
      dlssTotal: dlssTotal,
      dlssPercent: dlssPercent,
    );
    _outputFrameCount = frame;
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
            Expanded(flex: 4, child: _buildPreviewSection()),

            // 2. Progress bar
            _buildProgressSection(state),

            // 3. Log panel
            _buildLogSection(),

            // 4. Completion stats
            if (state.taskState == TaskState.done) ...[
              _buildCompletionStats(state),
              SizedBox(height: 12),
            ],
          ],
        );
      },
    );
  }

  Widget _buildPreviewSection() {
    return Container(
      margin: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: const Color(0xFF0A0A0A),
        border: Border.all(color: const Color(0xFF333333)),
        borderRadius: BorderRadius.circular(8),
      ),
      child: ClipRRect(
        borderRadius: BorderRadius.circular(8),
        child: _previewImage != null
            ? RawImage(image: _previewImage, fit: BoxFit.contain)
            : Center(
                child: Column(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    const Icon(Icons.image, color: Colors.white24, size: 48),
                    const SizedBox(height: 12),
                    Text(
                      context.strings.livePreview,
                      style: const TextStyle(
                        color: Colors.white38,
                        fontSize: 13,
                        letterSpacing: 0.5,
                      ),
                    ),
                  ],
                ),
              ),
      ),
    );
  }

  Widget _buildProgressSection(AppState state) {
    if (state.taskState == TaskState.idle) return const SizedBox.shrink();

    String statusTitle = context.strings.processing;
    Color statusColor = Colors.white;
    if (state.taskState == TaskState.done) {
      statusTitle = context.strings.completed;
      statusColor = Colors.green;
    } else if (state.taskState == TaskState.error) {
      statusTitle = context.strings.error;
      statusColor = Colors.red;
    } else if (state.taskState == TaskState.cancelling) {
      statusTitle = context.strings.cancellingTitle;
      statusColor = Colors.orange;
    }

    String etaText = '';
    if (state.taskState == TaskState.running &&
        _startTime != null &&
        state.currentFrame > 0 &&
        state.totalFrames > state.currentFrame) {
      final elapsed = DateTime.now().difference(_startTime!);
      final timePerFrame = elapsed.inMilliseconds / state.currentFrame;
      final remainingFrames = state.totalFrames - state.currentFrame;
      final etaMillis = timePerFrame * remainingFrames;
      final eta = Duration(milliseconds: etaMillis.round());
      etaText = context.strings.eta(
        '${eta.inMinutes}:${(eta.inSeconds % 60).toString().padLeft(2, '0')}',
      );
    }

    return Container(
      margin: const EdgeInsets.symmetric(horizontal: 12),
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: const Color(0xFF1E1E1E),
        border: Border.all(color: const Color(0xFF333333)),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text(
                statusTitle,
                style: TextStyle(
                  color: statusColor,
                  fontSize: 13,
                  fontWeight: FontWeight.w700,
                  letterSpacing: 1.0,
                ),
              ),
              if (etaText.isNotEmpty)
                Text(
                  etaText,
                  style: const TextStyle(
                    color: Colors.white54,
                    fontSize: 12,
                    fontFamily: 'monospace',
                  ),
                ),
            ],
          ),
          const SizedBox(height: 16),
          _buildProgressRow(
            context.strings.dlssPipeline,
            state.dlssCurrentFrame,
            state.dlssTotalFrames,
            state.dlssProgressPercent,
            const Color(0xFF00B050), // NVIDIA Green
          ),
          const SizedBox(height: 12),
          _buildProgressRow(
            context.strings.diskOutput,
            state.currentFrame,
            state.totalFrames,
            state.progressPercent,
            const Color(0xFF0078D7), // Tech Blue
          ),
        ],
      ),
    );
  }

  Widget _buildProgressRow(
    String label,
    int current,
    int total,
    double percent,
    Color color,
  ) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          children: [
            Text(
              label,
              style: const TextStyle(color: Colors.white70, fontSize: 12),
            ),
            Text(
              '$current / $total  (${(percent * 100).toStringAsFixed(1)}%)',
              style: const TextStyle(
                color: Colors.white,
                fontSize: 12,
                fontFamily: 'monospace',
              ),
            ),
          ],
        ),
        const SizedBox(height: 6),
        ClipRRect(
          borderRadius: BorderRadius.circular(4),
          child: TweenAnimationBuilder<double>(
            tween: Tween<double>(begin: 0, end: percent.clamp(0.0, 1.0)),
            duration: const Duration(milliseconds: 250),
            curve: Curves.easeOutCubic,
            builder: (context, value, _) {
              return LinearProgressIndicator(
                minHeight: 8,
                value: value,
                backgroundColor: Colors.white10,
                valueColor: AlwaysStoppedAnimation<Color>(color),
              );
            },
          ),
        ),
      ],
    );
  }

  Widget _buildLogSection() {
    final logCount = _logs.length;
    return Container(
      margin: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: const Color(0xFF0A0A0A),
        border: Border.all(color: const Color(0xFF333333)),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          InkWell(
            borderRadius: BorderRadius.circular(8),
            onTap: () => setState(() => _logsExpanded = !_logsExpanded),
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
              decoration: BoxDecoration(
                border: _logsExpanded
                    ? const Border(bottom: BorderSide(color: Color(0xFF333333)))
                    : null,
              ),
              child: Row(
                children: [
                  Icon(
                    _logsExpanded ? Icons.expand_more : Icons.chevron_right,
                    size: 16,
                    color: Colors.white38,
                  ),
                  const SizedBox(width: 6),
                  Text(
                    context.strings.terminalLog,
                    style: TextStyle(
                      color: Colors.white38,
                      fontSize: 11,
                      fontWeight: FontWeight.bold,
                      letterSpacing: 1.0,
                    ),
                  ),
                  const Spacer(),
                  Text(
                    logCount == 0
                        ? context.strings.noEntries
                        : context.strings.entries(logCount),
                    style: const TextStyle(color: Colors.white30, fontSize: 11),
                  ),
                ],
              ),
            ),
          ),
          if (_logsExpanded)
            SizedBox(
              height: 180,
              child: logCount == 0
                  ? Center(
                      child: Text(
                        context.strings.noTerminalOutputYet,
                        style: const TextStyle(color: Colors.white30, fontSize: 12),
                      ),
                    )
                  : ListView.builder(
                      padding: const EdgeInsets.symmetric(vertical: 8),
                      itemCount: logCount,
                      itemBuilder: (ctx, i) {
                        final log = _logs[i];
                        Color color = switch (log.level) {
                          'error' => Colors.redAccent,
                          'warn' || 'warning' => Colors.orangeAccent,
                          'debug' => Colors.white38,
                          _ => Colors.white70,
                        };
                        return Padding(
                          padding: const EdgeInsets.symmetric(
                            horizontal: 12,
                            vertical: 2,
                          ),
                          child: Text(
                            '[${log.level.padRight(5).toUpperCase()}] ${log.message}',
                            style: TextStyle(
                              color: color,
                              fontSize: 12,
                              fontFamily: 'monospace',
                            ),
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
      margin: const EdgeInsets.symmetric(horizontal: 12),
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
      decoration: BoxDecoration(
        color: const Color(0xFF00B050).withValues(alpha: 0.1),
        border: Border.all(
          color: const Color(0xFF00B050).withValues(alpha: 0.3),
        ),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.center,
        children: [
          const Icon(Icons.check_circle, color: Color(0xFF00B050), size: 28),
          const SizedBox(width: 16),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  context.strings.processCompleted,
                  style: TextStyle(
                    color: Color(0xFF00B050),
                    fontWeight: FontWeight.bold,
                    letterSpacing: 0.5,
                  ),
                ),
                const SizedBox(height: 4),
                Text(
                  context.strings.renderedFramesInSeconds(
                    _outputFrameCount,
                    duration.inSeconds,
                  ),
                  style: const TextStyle(color: Colors.white70, fontSize: 13),
                ),
              ],
            ),
          ),
          Align(
            alignment: Alignment.center,
            child: SizedBox(
              height: 38,
              child: FilledButton.icon(
                onPressed: state.outputDir.trim().isEmpty
                    ? null
                    : () => _openOutputFolder(state.outputDir),
                icon: const Icon(Icons.folder_open, size: 15),
                label: Text(context.strings.openFolder),
                style: FilledButton.styleFrom(
                  backgroundColor: const Color(0xFF00B050),
                  foregroundColor: Colors.white,
                  padding: const EdgeInsets.symmetric(horizontal: 16),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(12),
                  ),
                  textStyle: const TextStyle(
                    fontSize: 12,
                    fontWeight: FontWeight.w600,
                  ),
                  minimumSize: Size.zero,
                  tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }

  Future<void> _openOutputFolder(String outputDir) async {
    final dir = Directory(outputDir.trim()).absolute;
    AppLog.phase('detail', 'Open output folder requested', {'path': dir.path});
    if (!dir.existsSync()) {
      AppLog.phase('detail', 'Open output folder failed: missing directory', {
        'path': dir.path,
      });
      if (!mounted) return;
      await showErrorDialog(
        context,
        title: context.strings.outputFolderNotFoundTitle,
        message: context.strings.outputFolderNotFoundMessage,
        details: dir.path,
      );
      return;
    }

    try {
      if (Platform.isWindows) {
        final nativePath = dir.path.replaceAll('/', r'\');
        await Process.start('explorer.exe', [nativePath], runInShell: false);
      } else if (Platform.isMacOS) {
        await Process.start('open', [dir.path], runInShell: false);
      } else {
        await Process.start('xdg-open', [dir.path], runInShell: false);
      }
    } on ProcessException catch (e) {
      AppLog.phase('detail', 'Open output folder spawn failed', e.message);
      if (!mounted) return;
      await showErrorDialog(
        context,
        title: context.strings.failedOpenOutputFolderTitle,
        message: context.strings.failedOpenOutputFolderMessage,
        details: '${e.message}\n${dir.path}',
      );
    }
  }
}
