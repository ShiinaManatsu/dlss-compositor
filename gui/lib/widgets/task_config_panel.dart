import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../l10n/app_strings.dart';
import '../plugin/dlss_compositor.dart';
import '../state/app_state.dart';
import '../state/preferences.dart';
import '../util/app_log.dart';
import '../util/error_dialog.dart';
import '../util/win_file_dialog.dart';
import 'dlssconfig_picker.dart';

enum OverwriteMode { skip, overwrite, autoVersion }

enum OutputFormat { png16, exrLinear, proRes422Hq, h265Hevc }

enum ExrCompression { none, zip, dwaa }

extension OutputFormatExt on OutputFormat {
  String label(AppStrings strings) {
    switch (this) {
      case OutputFormat.png16:
        return strings.outputFormatPng16;
      case OutputFormat.exrLinear:
        return strings.outputFormatExr;
      case OutputFormat.proRes422Hq:
        return strings.outputFormatProRes422Hq;
      case OutputFormat.h265Hevc:
        return strings.outputFormatH265Hevc;
    }
  }

  String get value {
    switch (this) {
      case OutputFormat.png16:
        return 'png_16';
      case OutputFormat.exrLinear:
        return 'exr';
      case OutputFormat.proRes422Hq:
        return 'prores_422hq';
      case OutputFormat.h265Hevc:
        return 'h265_hevc';
    }
  }
}

extension ExrCompressionExt on ExrCompression {
  String label(AppStrings strings) {
    switch (this) {
      case ExrCompression.none:
        return strings.exrUncompressed;
      case ExrCompression.zip:
        return 'ZIP';
      case ExrCompression.dwaa:
        return 'DWAA';
    }
  }

  String get value {
    switch (this) {
      case ExrCompression.none:
        return 'none';
      case ExrCompression.zip:
        return 'zip';
      case ExrCompression.dwaa:
        return 'dwaa';
    }
  }
}

class TaskConfigPanel extends StatefulWidget {
  const TaskConfigPanel({super.key});

  @override
  State<TaskConfigPanel> createState() => _TaskConfigPanelState();
}

class _TaskConfigPanelState extends State<TaskConfigPanel> {
  final _templateController = TextEditingController();
  final _frameStartController = TextEditingController();
  final _frameEndController = TextEditingController();
  String? _lastConfigPathForDefaults;
  bool _prefsApplied = false;

  @override
  void initState() {
    super.initState();
    _templateController.addListener(_onTemplateChanged);
    _frameStartController.addListener(_onFrameRangeChanged);
    _frameEndController.addListener(_onFrameRangeChanged);
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    final state = Provider.of<AppState>(context, listen: false);
    final normalized = _stripKnownExtension(state.template);
    if (_templateController.text != normalized) {
      _templateController.text = normalized;
    }
  }

  @override
  void dispose() {
    _templateController.dispose();
    _frameStartController.dispose();
    _frameEndController.dispose();
    super.dispose();
  }

  void _onTemplateChanged() {
    final state = Provider.of<AppState>(context, listen: false);
    final normalized = _stripKnownExtension(_templateController.text);
    if (_templateController.text != normalized) {
      _templateController.value = TextEditingValue(
        text: normalized,
        selection: TextSelection.collapsed(offset: normalized.length),
      );
    }
    if (state.template != normalized) {
      state.setTaskConfig(template: normalized);
    }
  }

  void _onFrameRangeChanged() {
    final state = Provider.of<AppState>(context, listen: false);
    final start = _parseFrameField(_frameStartController.text);
    final end = _parseFrameField(_frameEndController.text);
    if (state.frameStart != start || state.frameEnd != end) {
      state.setTaskConfig(frameStart: start, frameEnd: end);
    }
  }

  int _parseFrameField(String value) {
    final trimmed = value.trim();
    if (trimmed.isEmpty) return -1;
    return int.tryParse(trimmed) ?? -1;
  }

  String _stripKnownExtension(String template) {
    return template.trim().replaceFirst(
      RegExp(r'\.(png|exr|mov|mp4)$', caseSensitive: false),
      '',
    );
  }

  String? _templateError(String text) {
    final template = text.trim();
    if (template.isEmpty) return context.strings.filenameTemplateRequired;
    if (template.contains('/') || template.contains('\\')) {
      return context.strings.filenameTemplateNoSeparators;
    }
    return null;
  }

  String? _frameRangeError(AppState state) {
    final startText = _frameStartController.text.trim();
    final endText = _frameEndController.text.trim();
    final start = _parseFrameField(startText);
    final end = _parseFrameField(endText);
    if (startText.isNotEmpty && start < 0) {
      return context.strings.invalidFrameRangeMessage;
    }
    if (endText.isNotEmpty && end < 0) {
      return context.strings.invalidFrameRangeMessage;
    }
    if (start >= 0 && end >= 0 && start > end) {
      return context.strings.invalidFrameRangeMessage;
    }
    if (_inputFrameCountInRange(state) == 0) {
      return context.strings.emptyFrameRangeMessage;
    }
    return null;
  }

  void _applyDefaults(AppState state, AppPreferences? prefs) {
    if (prefs != null && !_prefsApplied) {
      _prefsApplied = true;
      if (prefs.presetOverride != state.preset ||
          prefs.fgEnabled != state.fgEnabled ||
          prefs.fgMultiplier != state.fgMultiplier ||
          prefs.outputFormat != state.outputFormat ||
          prefs.pngQuality != state.pngQuality ||
          prefs.exrCompression != state.exrCompression ||
          prefs.exrCompressionLevel != state.exrCompressionLevel ||
          prefs.writerThreads != state.writerThreads ||
          prefs.writerBufferMultiplier != state.writerBufferMultiplier ||
          prefs.h265Cq != state.h265Cq ||
          prefs.h265Preset != state.h265Preset ||
          prefs.h265Multipass != state.h265Multipass ||
          prefs.h265SpatialAq != state.h265SpatialAq ||
          prefs.h265TemporalAq != state.h265TemporalAq ||
          prefs.h265Vfr != state.h265Vfr ||
          prefs.outputDirectory != state.outputDir) {
        Future.microtask(() {
          if (!mounted) return;
          state.setTaskConfig(
            preset: prefs.presetOverride,
            fgEnabled: prefs.fgEnabled,
            fgMultiplier: prefs.fgMultiplier,
            outputFormat: prefs.outputFormat,
            pngQuality: prefs.pngQuality,
            exrCompression: prefs.exrCompression,
            exrCompressionLevel: prefs.exrCompressionLevel,
            writerThreads: prefs.writerThreads,
            writerBufferMultiplier: prefs.writerBufferMultiplier,
            h265Cq: prefs.h265Cq,
            h265Preset: prefs.h265Preset,
            h265Multipass: prefs.h265Multipass,
            h265SpatialAq: prefs.h265SpatialAq,
            h265TemporalAq: prefs.h265TemporalAq,
            h265Vfr: prefs.h265Vfr,
            outputDir: prefs.outputDirectory,
          );
        });
      }
    } else if (prefs == null) {
      _prefsApplied = true;
    }

    final configPath = state.currentDlssConfigPath;
    if (configPath != null && configPath != _lastConfigPathForDefaults) {
      _lastConfigPathForDefaults = configPath;
      Future.microtask(() {
        if (mounted) {
          _frameStartController.clear();
          _frameEndController.clear();
          state.setTaskConfig(frameStart: -1, frameEnd: -1);
        }
      });
      if (state.outputDir.isEmpty) {
        final normalized = configPath.replaceAll('\\', '/');
        final slash = normalized.lastIndexOf('/');
        if (slash >= 0) {
          Future.microtask(() {
            if (mounted) {
              state.setTaskConfig(
                outputDir: '${normalized.substring(0, slash)}/output',
              );
            }
          });
        }
      }
    }
  }

  String _templateSample(AppState state) {
    final tmpl = _templateForFormat(state.template, state.outputFormat);
    return tmpl.replaceAll('####', '0001');
  }

  String _templateForFormat(String template, String format) {
    final trimmed = _stripKnownExtension(template).isEmpty
        ? 'frame_####'
        : _stripKnownExtension(template);
    final isVideo = format == 'prores_422hq' || format == 'h265_hevc';
    final base = isVideo
        ? trimmed
              .replaceAll('#', '')
              .replaceAll(RegExp(r'\{frame(?::0?\d+d)?\}'), '')
              .replaceAll(RegExp(r'_+'), '_')
              .replaceFirst(RegExp(r'_$'), '')
        : trimmed;
    final expectedExt = format == 'png_16'
        ? '.png'
        : format == 'prores_422hq'
        ? '.mov'
        : format == 'h265_hevc'
        ? '.mp4'
        : '.exr';
    return '${base.isEmpty ? 'output' : base}$expectedExt';
  }

  List<OutputFormat> _getAvailableFormats(bool isHdr) {
    if (isHdr) {
      return [
        OutputFormat.exrLinear,
        OutputFormat.proRes422Hq,
        OutputFormat.h265Hevc,
      ];
    }
    return [
      OutputFormat.png16,
      OutputFormat.exrLinear,
      OutputFormat.proRes422Hq,
      OutputFormat.h265Hevc,
    ];
  }

  String _autoPresetForQualityMode(AppState state) {
    final quality = '${state.currentDlssConfig?['quality_mode'] ?? ''}'
        .toLowerCase()
        .replaceAll('-', '_')
        .replaceAll(' ', '_');
    switch (quality) {
      case 'performance':
        return 'M';
      case 'ultra_performance':
        return 'L';
      default:
        return 'K';
    }
  }

  String _presetLabel(AppStrings strings, AppState state, String preset) {
    if (preset == 'auto') {
      return '${strings.autoPreset} (${_autoPresetForQualityMode(state)})';
    }
    return 'Preset $preset';
  }

  Future<void> _onStartPressed(BuildContext context) async {
    final appState = Provider.of<AppState>(context, listen: false);
    final prefs = Provider.of<AppPreferences>(context, listen: false);
    AppLog.phase('task', 'Start pressed');
    if (appState.currentDlssConfig == null) {
      AppLog.phase(
        'task',
        'Start rejected: invalid config',
        appState.configError,
      );
      await showErrorDialog(
        context,
        title: context.strings.noValidConfigTitle,
        message: context.strings.noValidConfigMessage,
        details: appState.configError,
      );
      return;
    }

    final templateError = _templateError(appState.template);
    if (templateError != null) {
      AppLog.phase('task', 'Start rejected: invalid template', templateError);
      await showErrorDialog(
        context,
        title: context.strings.invalidFilenameTemplateTitle,
        message: templateError,
      );
      return;
    }

    final outputDir = appState.outputDir.trim();
    if (outputDir.isEmpty) {
      AppLog.phase('task', 'Start rejected: missing output directory');
      await showErrorDialog(
        context,
        title: context.strings.outputDirectoryRequiredTitle,
        message: context.strings.outputDirectoryRequiredMessage,
      );
      return;
    }

    final frameRangeError = _frameRangeError(appState);
    if (frameRangeError != null) {
      AppLog.phase(
        'task',
        'Start rejected: invalid frame range',
        frameRangeError,
      );
      await showErrorDialog(
        context,
        title: context.strings.invalidFrameRangeTitle,
        message: frameRangeError,
      );
      return;
    }

    final taskParams = <String, dynamic>{
      'config': appState.currentDlssConfigPath,
      'preset': appState.preset,
      'fg_enabled':
          appState.fgEnabled &&
          appState.fgAvailable &&
          appState.fgMaxMultiplier >= 2,
      'fg_multiplier': appState.fgMultiplier,
      'output_format': appState.outputFormat,
      'png_quality': appState.pngQuality,
      'exr_compression': appState.exrCompression,
      'exr_compression_level': appState.exrCompressionLevel,
      'writer_threads': _effectiveWriterThreads(appState),
      'writer_buffer_multiplier': appState.writerBufferMultiplier,
      'h265_cq': appState.h265Cq,
      'h265_preset': appState.h265Preset,
      'h265_multipass': appState.h265Multipass,
      'h265_spatial_aq': appState.h265SpatialAq,
      'h265_temporal_aq': appState.h265TemporalAq,
      'h265_vfr': appState.h265Vfr,
      'output_dir': outputDir,
      'template': _templateForFormat(appState.template, appState.outputFormat),
      'output_template': _templateForFormat(
        appState.template,
        appState.outputFormat,
      ),
      'overwrite_mode': appState.overwriteMode,
      'frame_count': _expectedOutputFrameCount(appState),
    };
    if (appState.frameStart >= 0) {
      taskParams['frame_start'] = appState.frameStart;
    }
    if (appState.frameEnd >= 0) {
      taskParams['frame_end'] = appState.frameEnd;
    }

    AppLog.phase(
      'task',
      'Checking output collisions',
      AppLog.summarizeTaskParams(taskParams),
    );
    final collisions = await DlssCompositor.checkOutputCollisions(taskParams);
    final count = collisions['collisions'] as int? ?? 0;
    AppLog.phase('task', 'Output collision check complete', collisions);
    if (count > 0 && context.mounted) {
      final proceed = await showDialog<bool>(
        context: context,
        builder: (dialogContext) => AlertDialog(
          title: Text(context.strings.outputCollisionsTitle),
          content: Text(
            context.strings.outputCollisionsMessage(
              count,
              appState.overwriteMode,
            ),
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.of(dialogContext).pop(false),
              child: Text(context.strings.cancel),
            ),
            FilledButton(
              onPressed: () => Navigator.of(dialogContext).pop(true),
              child: Text(context.strings.continueLabel),
            ),
          ],
        ),
      );
      AppLog.phase('task', 'Collision dialog resolved', {'proceed': proceed});
      if (proceed != true) return;
    }

    prefs.setPresetOverride(appState.preset);
    prefs.setFgEnabled(appState.fgEnabled);
    prefs.setFgMultiplier(appState.fgMultiplier);
    prefs.setOutputFormat(appState.outputFormat);
    prefs.setPngQuality(appState.pngQuality);
    prefs.setExrCompression(appState.exrCompression);
    prefs.setExrCompressionLevel(appState.exrCompressionLevel);
    prefs.setWriterThreads(appState.writerThreads);
    prefs.setWriterBufferMultiplier(appState.writerBufferMultiplier);
    prefs.setH265Cq(appState.h265Cq);
    prefs.setH265Preset(appState.h265Preset);
    prefs.setH265Multipass(appState.h265Multipass);
    prefs.setH265SpatialAq(appState.h265SpatialAq);
    prefs.setH265TemporalAq(appState.h265TemporalAq);
    prefs.setH265Vfr(appState.h265Vfr);
    prefs.setOutputDirectory(outputDir);

    AppLog.phase('task', 'Sending setTask');
    final setOk = await DlssCompositor.setTask(taskParams);
    if (!setOk && context.mounted) {
      AppLog.phase('task', 'setTask rejected');
      await showErrorDialog(
        context,
        title: context.strings.backendRejectedTaskTitle,
        message: context.strings.backendRejectedTaskMessage,
        details: taskParams.toString(),
      );
      return;
    }

    AppLog.phase('task', 'Sending startTask');
    final startOk = await DlssCompositor.startTask();
    if (startOk) {
      appState.updateProgress(0, 0, 0.0);
      appState.setTaskState(TaskState.running, error: null);
      AppLog.phase('task', 'Task running');
    } else if (context.mounted) {
      AppLog.phase('task', 'startTask rejected');
      final message = context.strings.backendUnavailableMessage;
      appState.setTaskState(TaskState.error, error: message);
      await showErrorDialog(
        context,
        title: context.strings.backendUnavailableTitle,
        message: message,
        details: taskParams.toString(),
      );
    }
  }

  Future<void> _onCancelPressed(BuildContext context) async {
    final strings = context.strings;
    final appState = context.read<AppState>();
    if (appState.taskState != TaskState.running) return;

    AppLog.phase('task', 'Cancel pressed');
    appState.setTaskState(TaskState.cancelling, error: null);
    try {
      final ok = await DlssCompositor.cancelTask();
      if (!ok) {
        final message = strings.cancelRejectedMessage;
        AppLog.phase('task', 'cancelTask rejected');
        appState.setTaskState(TaskState.error, error: message);
        if (context.mounted) {
          await showErrorDialog(
            context,
            title: strings.cancelFailedTitle,
            message: message,
          );
        }
      }
    } catch (e) {
      final message = 'Cancel request failed: $e';
      AppLog.phase('task', 'cancelTask failed', e);
      appState.setTaskState(TaskState.error, error: message);
      if (context.mounted) {
        await showErrorDialog(
          context,
          title: strings.cancelFailedTitle,
          message: strings.cancelFailedMessage,
          details: '$e',
        );
      }
    }
  }

  int _expectedOutputFrameCount(AppState appState) {
    final inputFrames = _inputFrameCountInRange(appState);
    if (!appState.fgEnabled || inputFrames == 0) {
      return inputFrames;
    }
    return appState.fgMultiplier * inputFrames - (appState.fgMultiplier - 1);
  }

  int _inputFrameCountInRange(AppState appState) {
    final frames = appState.currentDlssConfig?['frames'];
    if (frames is List) {
      final start = appState.frameStart >= 0 ? appState.frameStart : 0;
      final end = appState.frameEnd >= 0
          ? appState.frameEnd
          : frames.length - 1;
      if (frames.isEmpty || start > end || start >= frames.length || end < 0) {
        return 0;
      }
      final clampedStart = start.clamp(0, frames.length - 1);
      final clampedEnd = end.clamp(0, frames.length - 1);
      return clampedEnd - clampedStart + 1;
    }
    if (frames is Map) {
      var count = 0;
      for (final key in frames.keys) {
        final frame = int.tryParse('$key');
        if (frame == null) continue;
        if (appState.frameStart >= 0 && frame < appState.frameStart) continue;
        if (appState.frameEnd >= 0 && frame > appState.frameEnd) continue;
        count++;
      }
      return count;
    }
    return 0;
  }

  int _effectiveWriterThreads(AppState state) {
    if (state.writerThreads > 0) {
      return state.writerThreads.clamp(1, 32);
    }
    final resolution = state.currentDlssConfig?['output_resolution'];
    final width = resolution is Map ? resolution['width'] as int? : null;
    final height = resolution is Map ? resolution['height'] as int? : null;
    if (width == null || height == null || width <= 0 || height <= 0) {
      return 4;
    }
    const targetDiskMiBPerSecond = 200.0;
    final frameMiB = width * height * 8 / (1024 * 1024);
    if (frameMiB <= 0) return 4;
    return (targetDiskMiBPerSecond / frameMiB).round().clamp(1, 8);
  }

  ({int width, int height})? _outputResolution(AppState state) {
    final resolution = state.currentDlssConfig?['output_resolution'];
    final width = resolution is Map ? resolution['width'] as int? : null;
    final height = resolution is Map ? resolution['height'] as int? : null;
    if (width == null || height == null || width <= 0 || height <= 0) {
      return null;
    }
    return (width: width, height: height);
  }

  String _estimatedBufferMemory(AppState state, int maxQueuedFrames) {
    final resolution = _outputResolution(state);
    if (resolution == null || maxQueuedFrames <= 0) {
      return 'unknown';
    }
    final isVideo =
        state.outputFormat == 'prores_422hq' ||
        state.outputFormat == 'h265_hevc';
    final bytesPerPixel = isVideo ? 4 : 8;
    final bytes =
        resolution.width * resolution.height * bytesPerPixel * maxQueuedFrames;
    final gib = bytes / (1024 * 1024 * 1024);
    if (gib >= 1.0) {
      return '${gib.toStringAsFixed(2)} GB';
    }
    final mib = bytes / (1024 * 1024);
    return '${mib.toStringAsFixed(0)} MB';
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<AppState>(
      builder: (context, state, _) {
        AppPreferences? prefs;
        try {
          prefs = context.watch<AppPreferences>();
        } on ProviderNotFoundException {
          prefs = null;
        }
        _applyDefaults(state, prefs);
        final isHdr = state.currentDlssConfig?['is_hdr'] as bool? ?? false;
        final availableFormats = _getAvailableFormats(isHdr);

        if (!availableFormats
            .map((f) => f.value)
            .contains(state.outputFormat)) {
          Future.microtask(() {
            state.setTaskConfig(outputFormat: availableFormats.first.value);
          });
        }

        return SingleChildScrollView(
          padding: const EdgeInsets.all(12),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              const DlssConfigPicker(),
              const SizedBox(height: 24),
              _buildSection(
                context.strings.settings,
                _buildSettings(state, availableFormats),
              ),
              const SizedBox(height: 24),
              SizedBox(
                height: 48,
                child: FilledButton.icon(
                  onPressed: state.taskState == TaskState.running
                      ? () => _onCancelPressed(context)
                      : state.currentDlssConfig != null &&
                            state.taskState != TaskState.cancelling
                      ? () => _onStartPressed(context)
                      : null,
                  icon: Icon(
                    state.taskState == TaskState.running
                        ? Icons.stop
                        : state.taskState == TaskState.cancelling
                        ? Icons.hourglass_empty
                        : Icons.play_arrow,
                  ),
                  label: Text(
                    state.taskState == TaskState.running
                        ? context.strings.cancel
                        : state.taskState == TaskState.cancelling
                        ? context.strings.cancellingUpper
                        : context.strings.startProcessing,
                    style: const TextStyle(
                      fontWeight: FontWeight.w600,
                      letterSpacing: 0.5,
                    ),
                  ),
                  style: FilledButton.styleFrom(
                    backgroundColor: state.taskState == TaskState.running
                        ? const Color(0xFFC62828)
                        : const Color(0xFF00B050),
                    foregroundColor: Colors.white,
                    shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(4),
                    ),
                  ),
                ),
              ),
            ],
          ),
        );
      },
    );
  }

  Widget _buildSection(String title, Widget child) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          title,
          style: const TextStyle(
            fontWeight: FontWeight.w600,
            fontSize: 14,
            color: Colors.white70,
            letterSpacing: 0.5,
          ),
        ),
        const SizedBox(height: 12),
        Container(
          padding: const EdgeInsets.all(16),
          decoration: BoxDecoration(
            color: const Color(0xFF1E1E1E),
            borderRadius: BorderRadius.circular(8),
            border: Border.all(color: const Color(0xFF333333)),
          ),
          child: child,
        ),
      ],
    );
  }

  Widget _buildFormRow(String label, Widget child) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 12.0),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.center,
        children: [
          SizedBox(
            width: 110,
            child: Text(
              label,
              style: const TextStyle(fontSize: 13, color: Colors.white60),
            ),
          ),
          Expanded(child: child),
        ],
      ),
    );
  }

  Widget _buildSettings(AppState state, List<OutputFormat> formats) {
    final strings = context.strings;
    final fgCanRun = state.fgAvailable && state.fgMaxMultiplier >= 2;
    final isVideoOutput =
        state.outputFormat == 'prores_422hq' ||
        state.outputFormat == 'h265_hevc';
    final multipliers = [
      for (
        var multiplier = 2;
        multiplier <= state.fgMaxMultiplier;
        multiplier++
      )
        multiplier,
    ];
    final writerThreads = _effectiveWriterThreads(state);
    final bufferMultiplier = state.writerBufferMultiplier;
    final maxQueuedFrames = writerThreads * bufferMultiplier;
    final estimatedBufferMemory = _estimatedBufferMemory(
      state,
      maxQueuedFrames,
    );

    return Column(
      children: [
        _buildFormRow(
          strings.dlssPreset,
          DropdownButtonFormField<String>(
            initialValue: state.preset,
            isDense: true,
            decoration: const InputDecoration(
              isDense: true,
              contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 8),
              border: OutlineInputBorder(),
            ),
            items: ['auto', 'J', 'K', 'L', 'M']
                .map(
                  (p) => DropdownMenuItem(
                    value: p,
                    child: Text(
                      _presetLabel(strings, state, p),
                      style: const TextStyle(fontSize: 13),
                    ),
                  ),
                )
                .toList(),
            onChanged: (v) => state.setTaskConfig(preset: v!),
          ),
        ),
        _buildFormRow(
          strings.frameGen,
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Switch(
                value: state.fgEnabled,
                onChanged: fgCanRun
                    ? (v) => state.setTaskConfig(fgEnabled: v)
                    : null,
              ),
              const SizedBox(width: 8),
              if (!fgCanRun)
                Expanded(
                  child: Text(
                    strings.unavailableOnThisDevice,
                    style: TextStyle(
                      fontSize: 12,
                      color: Colors.orange.shade300,
                    ),
                    overflow: TextOverflow.ellipsis,
                  ),
                )
              else if (state.fgEnabled)
                Expanded(
                  child: Wrap(
                    spacing: 4,
                    runSpacing: 4,
                    children: [
                      for (final multiplier in multipliers)
                        ChoiceChip(
                          label: Text(
                            '${multiplier}x',
                            style: const TextStyle(fontSize: 12),
                          ),
                          selected: state.fgMultiplier == multiplier,
                          onSelected: (_) =>
                              state.setTaskConfig(fgMultiplier: multiplier),
                          visualDensity: VisualDensity.compact,
                          materialTapTargetSize:
                              MaterialTapTargetSize.shrinkWrap,
                        ),
                    ],
                  ),
                ),
            ],
          ),
        ),
        _buildFormRow(
          strings.frameRange,
          Row(
            children: [
              Expanded(
                child: TextField(
                  key: const Key('frameRangeStartField'),
                  controller: _frameStartController,
                  keyboardType: TextInputType.number,
                  style: const TextStyle(fontSize: 13, fontFamily: 'monospace'),
                  decoration: InputDecoration(
                    isDense: true,
                    contentPadding: const EdgeInsets.symmetric(
                      horizontal: 12,
                      vertical: 8,
                    ),
                    border: const OutlineInputBorder(),
                    hintText: strings.frameRangeStartHint,
                  ),
                ),
              ),
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 8),
                child: Text(
                  strings.frameRangeTo,
                  style: const TextStyle(color: Colors.white38),
                ),
              ),
              Expanded(
                child: TextField(
                  key: const Key('frameRangeEndField'),
                  controller: _frameEndController,
                  keyboardType: TextInputType.number,
                  style: const TextStyle(fontSize: 13, fontFamily: 'monospace'),
                  decoration: InputDecoration(
                    isDense: true,
                    contentPadding: const EdgeInsets.symmetric(
                      horizontal: 12,
                      vertical: 8,
                    ),
                    border: const OutlineInputBorder(),
                    hintText: strings.frameRangeEndHint,
                  ),
                ),
              ),
              const SizedBox(width: 10),
              Text(
                strings.frameRangeSummary(_inputFrameCountInRange(state)),
                style: const TextStyle(fontSize: 12, color: Colors.white38),
              ),
            ],
          ),
        ),
        const Divider(height: 24, color: Color(0xFF333333)),
        _buildFormRow(
          strings.format,
          DropdownButtonFormField<String>(
            initialValue:
                formats.map((f) => f.value).contains(state.outputFormat)
                ? state.outputFormat
                : formats.first.value,
            isExpanded: true,
            isDense: true,
            decoration: const InputDecoration(
              isDense: true,
              contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 8),
              border: OutlineInputBorder(),
            ),
            items: formats
                .map(
                  (f) => DropdownMenuItem(
                    value: f.value,
                    child: Text(
                      f.label(strings),
                      style: const TextStyle(fontSize: 13),
                    ),
                  ),
                )
                .toList(),
            onChanged: (v) => state.setTaskConfig(outputFormat: v),
          ),
        ),
        if (state.outputFormat == 'png_16')
          _buildPercentSlider(
            strings.pngQuality,
            state.pngQuality,
            (v) => state.setTaskConfig(pngQuality: v),
          ),
        if (state.outputFormat == 'exr') ...[
          _buildFormRow(
            strings.exrCompression,
            DropdownButtonFormField<String>(
              initialValue:
                  ExrCompression.values
                      .map((c) => c.value)
                      .contains(state.exrCompression)
                  ? state.exrCompression
                  : ExrCompression.zip.value,
              isExpanded: true,
              isDense: true,
              decoration: const InputDecoration(
                isDense: true,
                contentPadding: EdgeInsets.symmetric(
                  horizontal: 12,
                  vertical: 8,
                ),
                border: OutlineInputBorder(),
              ),
              items: ExrCompression.values
                  .map(
                    (c) => DropdownMenuItem(
                      value: c.value,
                      child: Text(
                        c.label(strings),
                        style: const TextStyle(fontSize: 13),
                      ),
                    ),
                  )
                  .toList(),
              onChanged: (v) => state.setTaskConfig(exrCompression: v),
            ),
          ),
          if (state.exrCompression != ExrCompression.none.value)
            _buildPercentSlider(
              strings.compression,
              state.exrCompressionLevel,
              (v) => state.setTaskConfig(exrCompressionLevel: v),
            ),
        ],
        if (state.outputFormat == 'h265_hevc') ...[
          _buildIntSlider(
            strings.h265Quality,
            state.h265Cq,
            min: 0,
            max: 30,
            onChanged: (v) => state.setTaskConfig(h265Cq: v),
          ),
          _buildFormRow(
            strings.h265Preset,
            DropdownButtonFormField<String>(
              initialValue: ['p3', 'p5', 'p7'].contains(state.h265Preset)
                  ? state.h265Preset
                  : 'p5',
              isExpanded: true,
              isDense: true,
              decoration: const InputDecoration(
                isDense: true,
                contentPadding: EdgeInsets.symmetric(
                  horizontal: 12,
                  vertical: 8,
                ),
                border: OutlineInputBorder(),
              ),
              items: [
                DropdownMenuItem(
                  value: 'p3',
                  child: Text(strings.h265PresetFast),
                ),
                DropdownMenuItem(
                  value: 'p5',
                  child: Text(strings.h265PresetBalanced),
                ),
                DropdownMenuItem(
                  value: 'p7',
                  child: Text(strings.h265PresetQuality),
                ),
              ],
              onChanged: (v) => state.setTaskConfig(h265Preset: v),
            ),
          ),
          _buildFormRow(
            strings.h265Multipass,
            DropdownButtonFormField<String>(
              initialValue:
                  ['disabled', 'qres', 'fullres'].contains(state.h265Multipass)
                  ? state.h265Multipass
                  : 'qres',
              isExpanded: true,
              isDense: true,
              decoration: const InputDecoration(
                isDense: true,
                contentPadding: EdgeInsets.symmetric(
                  horizontal: 12,
                  vertical: 8,
                ),
                border: OutlineInputBorder(),
              ),
              items: [
                DropdownMenuItem(
                  value: 'disabled',
                  child: Text(strings.h265MultipassOff),
                ),
                DropdownMenuItem(
                  value: 'qres',
                  child: Text(strings.h265MultipassQres),
                ),
                DropdownMenuItem(
                  value: 'fullres',
                  child: Text(strings.h265MultipassFullres),
                ),
              ],
              onChanged: (v) => state.setTaskConfig(h265Multipass: v),
            ),
          ),
          _buildFormRow(
            'HEVC AQ',
            Wrap(
              spacing: 8,
              runSpacing: 4,
              children: [
                FilterChip(
                  label: Text(strings.h265SpatialAq),
                  selected: state.h265SpatialAq,
                  onSelected: (v) => state.setTaskConfig(h265SpatialAq: v),
                  visualDensity: VisualDensity.compact,
                ),
                FilterChip(
                  label: Text(strings.h265TemporalAq),
                  selected: state.h265TemporalAq,
                  onSelected: (v) => state.setTaskConfig(h265TemporalAq: v),
                  visualDensity: VisualDensity.compact,
                ),
                FilterChip(
                  label: Text(strings.h265Vfr),
                  selected: state.h265Vfr,
                  onSelected: (v) => state.setTaskConfig(h265Vfr: v),
                  visualDensity: VisualDensity.compact,
                ),
              ],
            ),
          ),
          Padding(
            padding: const EdgeInsets.only(left: 110, bottom: 6),
            child: Text(
              strings.h265VfrNote,
              style: const TextStyle(fontSize: 11, color: Colors.white38),
            ),
          ),
        ],
        _buildFormRow(
          strings.outputDir,
          Row(
            children: [
              Expanded(
                child: Container(
                  padding: const EdgeInsets.symmetric(
                    horizontal: 12,
                    vertical: 8,
                  ),
                  decoration: BoxDecoration(
                    border: Border.all(color: Colors.white24),
                    borderRadius: BorderRadius.circular(4),
                  ),
                  child: Text(
                    state.outputDir.isEmpty
                        ? strings.selectFolder
                        : state.outputDir,
                    style: TextStyle(
                      fontSize: 13,
                      fontFamily: 'monospace',
                      color: state.outputDir.isEmpty
                          ? Colors.white38
                          : Colors.white70,
                    ),
                    overflow: TextOverflow.ellipsis,
                  ),
                ),
              ),
              const SizedBox(width: 8),
              IconButton.outlined(
                icon: const Icon(Icons.folder, size: 16),
                onPressed: () async {
                  final dir = selectDirectory(
                    title: strings.selectOutputDirectoryTitle,
                  );
                  if (dir != null) state.setTaskConfig(outputDir: dir);
                },
              ),
            ],
          ),
        ),
        _buildFormRow(
          strings.template,
          Row(
            children: [
              Expanded(
                child: TextField(
                  key: const Key('filenameTemplateField'),
                  controller: _templateController,
                  style: const TextStyle(fontSize: 13, fontFamily: 'monospace'),
                  decoration: InputDecoration(
                    isDense: true,
                    contentPadding: const EdgeInsets.symmetric(
                      horizontal: 12,
                      vertical: 8,
                    ),
                    border: const OutlineInputBorder(),
                    hintText: strings.templateHint,
                  ),
                ),
              ),
              const SizedBox(width: 12),
              Flexible(
                child: Text(
                  strings.templatePreview(_templateSample(state)),
                  style: const TextStyle(
                    fontSize: 12,
                    color: Colors.white38,
                    fontFamily: 'monospace',
                  ),
                  overflow: TextOverflow.ellipsis,
                ),
              ),
            ],
          ),
        ),
        _buildFormRow(
          strings.onConflict,
          DropdownButtonFormField<String>(
            initialValue: state.overwriteMode,
            isExpanded: true,
            isDense: true,
            decoration: const InputDecoration(
              isDense: true,
              contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 8),
              border: OutlineInputBorder(),
            ),
            items: [
              DropdownMenuItem(
                value: 'skip',
                child: Text(
                  strings.skipExisting,
                  style: const TextStyle(fontSize: 13),
                ),
              ),
              DropdownMenuItem(
                value: 'overwrite',
                child: Text(
                  strings.overwrite,
                  style: const TextStyle(fontSize: 13),
                ),
              ),
              DropdownMenuItem(
                value: 'autoVersion',
                child: Text(
                  strings.autoVersion,
                  style: const TextStyle(fontSize: 13),
                ),
              ),
            ],
            onChanged: (v) => state.setTaskConfig(overwriteMode: v!),
          ),
        ),
        const Divider(height: 24, color: Color(0xFF333333)),
        _buildIntSlider(
          strings.diskWrites,
          writerThreads,
          min: 1,
          max: 16,
          onChanged: (v) => state.setTaskConfig(writerThreads: v),
        ),
        _buildIntSlider(
          strings.bufferSize,
          bufferMultiplier,
          min: 1,
          max: isVideoOutput ? 64 : 10,
          onChanged: (v) => state.setTaskConfig(writerBufferMultiplier: v),
        ),
        Padding(
          padding: const EdgeInsets.only(left: 110),
          child: Text(
            strings.writeBufferSummary(
              writerThreads,
              maxQueuedFrames,
              estimatedBufferMemory,
            ),
            style: const TextStyle(fontSize: 11, color: Colors.white38),
          ),
        ),
      ],
    );
  }

  Widget _buildPercentSlider(
    String label,
    int value,
    ValueChanged<int> onChanged,
  ) {
    return _buildFormRow(
      label,
      Row(
        children: [
          Expanded(
            child: Slider(
              value: value.toDouble(),
              min: 0,
              max: 100,
              divisions: 100,
              label: '$value%',
              onChanged: (v) => onChanged(v.round()),
            ),
          ),
          SizedBox(
            width: 48,
            child: Text(
              '$value%',
              textAlign: TextAlign.right,
              style: const TextStyle(
                color: Colors.white70,
                fontSize: 12,
                fontFamily: 'monospace',
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildIntSlider(
    String label,
    int value, {
    required int min,
    required int max,
    required ValueChanged<int> onChanged,
  }) {
    final clamped = value.clamp(min, max);
    return _buildFormRow(
      label,
      Row(
        children: [
          Expanded(
            child: Slider(
              value: clamped.toDouble(),
              min: min.toDouble(),
              max: max.toDouble(),
              divisions: max - min,
              label: '$clamped',
              onChanged: (v) => onChanged(v.round()),
            ),
          ),
          SizedBox(
            width: 48,
            child: Text(
              '$clamped',
              textAlign: TextAlign.right,
              style: const TextStyle(
                color: Colors.white70,
                fontSize: 12,
                fontFamily: 'monospace',
              ),
            ),
          ),
        ],
      ),
    );
  }
}
