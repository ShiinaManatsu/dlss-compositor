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
enum OutputFormat { png16, exrLinear, proRes422Hq }
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
  String? _lastConfigPathForDefaults;
  bool _prefsApplied = false;

  @override
  void initState() {
    super.initState();
    _templateController.addListener(_onTemplateChanged);
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

  String _stripKnownExtension(String template) {
    return template.trim().replaceFirst(
      RegExp(r'\.(png|exr|mov)$', caseSensitive: false),
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
    final base = format == 'prores_422hq'
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
            : '.exr';
    return '${base.isEmpty ? 'output' : base}$expectedExt';
  }

  List<OutputFormat> _getAvailableFormats(bool isHdr) {
    if (isHdr) {
      return [OutputFormat.exrLinear, OutputFormat.proRes422Hq];
    }
    return [
      OutputFormat.png16,
      OutputFormat.exrLinear,
      OutputFormat.proRes422Hq,
    ];
  }

  Future<void> _onStartPressed(BuildContext context) async {
    final appState = Provider.of<AppState>(context, listen: false);
    final prefs = Provider.of<AppPreferences>(context, listen: false);
    AppLog.phase('task', 'Start pressed');
    if (appState.currentDlssConfig == null) {
      AppLog.phase('task', 'Start rejected: invalid config', appState.configError);
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

    final taskParams = {
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
      'output_dir': outputDir,
      'template': _templateForFormat(appState.template, appState.outputFormat),
      'output_template': _templateForFormat(
        appState.template,
        appState.outputFormat,
      ),
      'overwrite_mode': appState.overwriteMode,
      'frame_count': _expectedOutputFrameCount(appState),
    };

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
    final frames = appState.currentDlssConfig?['frames'];
    final inputFrames = frames is List
        ? frames.length
        : frames is Map
            ? frames.length
            : 0;
    if (!appState.fgEnabled || inputFrames == 0) {
      return inputFrames;
    }
    return appState.fgMultiplier * inputFrames - (appState.fgMultiplier - 1);
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

        if (!availableFormats.map((f) => f.value).contains(state.outputFormat)) {
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
    final multipliers = [
      for (var multiplier = 2; multiplier <= state.fgMaxMultiplier; multiplier++)
        multiplier,
    ];
    final writerThreads = _effectiveWriterThreads(state);
    final bufferMultiplier = state.writerBufferMultiplier;
    final maxQueuedFrames = writerThreads * bufferMultiplier;

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
                      p == 'auto' ? strings.autoPreset : 'Preset $p',
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
                          onSelected: (_) => state.setTaskConfig(
                            fgMultiplier: multiplier,
                          ),
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
        const Divider(height: 24, color: Color(0xFF333333)),
        _buildFormRow(
          strings.format,
          DropdownButtonFormField<String>(
            initialValue: formats.map((f) => f.value).contains(state.outputFormat)
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
                  ExrCompression.values.map((c) => c.value).contains(
                        state.exrCompression,
                      )
                  ? state.exrCompression
                  : ExrCompression.zip.value,
              isExpanded: true,
              isDense: true,
              decoration: const InputDecoration(
                isDense: true,
                contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 8),
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
          max: 10,
          onChanged: (v) => state.setTaskConfig(writerBufferMultiplier: v),
        ),
        Padding(
          padding: const EdgeInsets.only(left: 110),
          child: Text(
            strings.writeBufferSummary(writerThreads, maxQueuedFrames),
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
