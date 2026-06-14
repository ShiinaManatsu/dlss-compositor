import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../state/app_state.dart';
import '../state/preferences.dart';
import '../plugin/dlss_compositor.dart';
import '../util/error_dialog.dart';
import '../util/win_file_dialog.dart';
import '../util/app_log.dart';
import 'dlssconfig_picker.dart';

enum OverwriteMode { skip, overwrite, autoVersion }
enum OutputFormat { png16, exrLinear, proRes422Hq }
enum ExrCompression { none, zip, dwaa }

extension OutputFormatExt on OutputFormat {
  String get label {
    switch (this) {
      case OutputFormat.png16:       return 'PNG (16-bit)';
      case OutputFormat.exrLinear:   return 'EXR (scene-linear)';
      case OutputFormat.proRes422Hq: return 'ProRes 422 HQ (.mov)';
    }
  }
  String get value {
    switch (this) {
      case OutputFormat.png16:       return 'png_16';
      case OutputFormat.exrLinear:   return 'exr';
      case OutputFormat.proRes422Hq: return 'prores_422hq';
    }
  }
}

extension ExrCompressionExt on ExrCompression {
  String get label {
    switch (this) {
      case ExrCompression.none: return 'Uncompressed';
      case ExrCompression.zip:  return 'ZIP';
      case ExrCompression.dwaa: return 'DWAA';
    }
  }

  String get value {
    switch (this) {
      case ExrCompression.none: return 'none';
      case ExrCompression.zip:  return 'zip';
      case ExrCompression.dwaa: return 'dwaa';
    }
  }
}

/// Left-side task configuration panel.
/// Contains: dlssconfig picker + readonly info + preset/FG config + output settings + Start button
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
    return template.trim().replaceFirst(RegExp(r'\.(png|exr|mov)$', caseSensitive: false), '');
  }

  String? _templateError(String text) {
    final template = text.trim();
    if (template.isEmpty) return 'Filename template is required';
    if (template.contains('/') || template.contains('\\')) {
      return 'Filename template must not contain path separators';
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
              state.setTaskConfig(outputDir: '${normalized.substring(0, slash)}/output');
            }
          });
        }
      }
    }
  }

  String _templateSample(AppState state) {
    final tmpl = _templateForFormat(state.template, state.outputFormat);
    final formatted = tmpl.replaceAll('####', '0001');
    return formatted;
  }

  String _templateForFormat(String template, String format) {
    final trimmed = _stripKnownExtension(template).isEmpty ? 'frame_####' : _stripKnownExtension(template);
    final base = format == 'prores_422hq'
        ? trimmed.replaceAll('#', '').replaceAll(RegExp(r'\{frame(?::0?\d+d)?\}'), '').replaceAll(RegExp(r'_+'), '_').replaceFirst(RegExp(r'_$'), '')
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
      // HDR: EXR for scene-linear interchange, ProRes 422 HQ for display-ready review/export.
      return [OutputFormat.exrLinear, OutputFormat.proRes422Hq];
    } else {
      // LDR: PNG, EXR, ProRes 422 HQ.
      return [OutputFormat.png16, OutputFormat.exrLinear, OutputFormat.proRes422Hq];
    }
    // NOTE: Additional formats reserved for future releases - see docs/cli_reference.md.
  }

  Future<void> _onStartPressed(BuildContext context) async {
    final appState = Provider.of<AppState>(context, listen: false);
    final prefs = Provider.of<AppPreferences>(context, listen: false);
    AppLog.phase('task', 'Start pressed');
    if (appState.currentDlssConfig == null) {
      AppLog.phase('task', 'Start rejected: invalid config', appState.configError);
      await showErrorDialog(
        context,
        title: 'No valid dlssconfig.json',
        message: 'Please select a valid dlssconfig.json before starting.',
        details: appState.configError,
      );
      return;
    }

    final templateError = _templateError(appState.template);
    if (templateError != null) {
      AppLog.phase('task', 'Start rejected: invalid template', templateError);
      await showErrorDialog(
        context,
        title: 'Invalid filename template',
        message: templateError,
      );
      return;
    }

    final outputDir = appState.outputDir.trim();
    if (outputDir.isEmpty) {
      AppLog.phase('task', 'Start rejected: missing output directory');
      await showErrorDialog(
        context,
        title: 'Output directory required',
        message: 'Please select an output directory before starting.',
      );
      return;
    }

    final taskParams = {
      'config': appState.currentDlssConfigPath,
      'preset': appState.preset,
      'fg_enabled': appState.fgEnabled && appState.fgAvailable && appState.fgMaxMultiplier >= 2,
      'fg_multiplier': appState.fgMultiplier,
      'output_format': appState.outputFormat,
      'png_quality': appState.pngQuality,
      'exr_compression': appState.exrCompression,
      'exr_compression_level': appState.exrCompressionLevel,
      'output_dir': outputDir,
      'template': _templateForFormat(appState.template, appState.outputFormat),
      'output_template': _templateForFormat(appState.template, appState.outputFormat),
      'overwrite_mode': appState.overwriteMode,
      'frame_count': _expectedOutputFrameCount(appState),
    };

    AppLog.phase('task', 'Checking output collisions', AppLog.summarizeTaskParams(taskParams));
    final collisions = await DlssCompositor.checkOutputCollisions(taskParams);
    final count = collisions['collisions'] as int? ?? 0;
    AppLog.phase('task', 'Output collision check complete', collisions);
    if (count > 0 && context.mounted) {
      final proceed = await showDialog<bool>(
        context: context,
        builder: (dialogContext) => AlertDialog(
          title: const Text('Output collisions detected'),
          content: Text('$count output files may conflict. Mode: ${appState.overwriteMode}. Continue?'),
          actions: [
            TextButton(
              onPressed: () => Navigator.of(dialogContext).pop(false),
              child: const Text('Cancel'),
            ),
            FilledButton(
              onPressed: () => Navigator.of(dialogContext).pop(true),
              child: const Text('Continue'),
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
    prefs.setOutputDirectory(outputDir);

    AppLog.phase('task', 'Sending setTask');
    final setOk = await DlssCompositor.setTask(taskParams);
    if (!setOk && context.mounted) {
      AppLog.phase('task', 'setTask rejected');
      await showErrorDialog(
        context,
        title: 'Backend rejected task',
        message: 'Failed to set task parameters.',
        details: taskParams.toString(),
      );
      return;
    }

    AppLog.phase('task', 'Sending startTask');
    final startOk = await DlssCompositor.startTask();
    if (startOk) {
      // Reset state for new task
      appState.updateProgress(0, 0, 0.0);
      appState.setTaskState(TaskState.running, error: null);
      AppLog.phase('task', 'Task running');
    } else if (context.mounted) {
      AppLog.phase('task', 'startTask rejected');
      const message = 'The processing backend could not be started. Check the root logs for details.';
      appState.setTaskState(TaskState.error, error: message);
      await showErrorDialog(
        context,
        title: 'Processing backend unavailable',
        message: message,
        details: taskParams.toString(),
      );
    }
  }

  Future<void> _onCancelPressed(BuildContext context) async {
    final appState = context.read<AppState>();
    if (appState.taskState != TaskState.running) return;

    AppLog.phase('task', 'Cancel pressed');
    appState.setTaskState(TaskState.cancelling, error: null);
    try {
      final ok = await DlssCompositor.cancelTask();
      if (!ok) {
        const message = 'The processing backend did not accept the cancel request.';
        AppLog.phase('task', 'cancelTask rejected');
        appState.setTaskState(TaskState.error, error: message);
        if (context.mounted) {
          await showErrorDialog(
            context,
            title: 'Cancel failed',
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
          title: 'Cancel failed',
          message: 'The cancel request failed.',
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

        // Reset format if not available for current HDR/LDR mode
        if (!availableFormats.map((f) => f.value).contains(state.outputFormat)) {
          // Use Future.microtask to avoid setState during build
          Future.microtask(() {
            state.setTaskConfig(outputFormat: availableFormats.first.value);
          });
        }

        return SingleChildScrollView(
          padding: const EdgeInsets.all(12),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              // 1. dlssconfig picker
              const DlssConfigPicker(),
              const SizedBox(height: 24),

              // 2. Preset selector
              _buildSection('Settings', _buildSettings(state, availableFormats)),
              const SizedBox(height: 24),

              // Start / cancel button
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
                        ? 'CANCEL'
                        : state.taskState == TaskState.cancelling
                            ? 'CANCELLING...'
                            : 'START PROCESSING',
                    style: const TextStyle(fontWeight: FontWeight.w600, letterSpacing: 0.5),
                  ),
                  style: FilledButton.styleFrom(
                    backgroundColor: state.taskState == TaskState.running
                        ? const Color(0xFFC62828)
                        : const Color(0xFF00B050),
                    foregroundColor: Colors.white,
                    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(4)),
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
        Text(title, style: const TextStyle(fontWeight: FontWeight.w600, fontSize: 14, color: Colors.white70, letterSpacing: 0.5)),
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
            child: Text(label, style: const TextStyle(fontSize: 13, color: Colors.white60)),
          ),
          Expanded(child: child),
        ],
      ),
    );
  }

  Widget _buildSettings(AppState state, List<OutputFormat> formats) {
    final fgCanRun = state.fgAvailable && state.fgMaxMultiplier >= 2;
    final multipliers = [
      for (var multiplier = 2; multiplier <= state.fgMaxMultiplier; multiplier++) multiplier,
    ];

    return Column(
      children: [
        _buildFormRow(
          'DLSS Preset',
          DropdownButtonFormField<String>(
            initialValue: state.preset,
            isDense: true,
            decoration: const InputDecoration(
              isDense: true,
              contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 8),
              border: OutlineInputBorder(),
            ),
            items: ['auto', 'J', 'K', 'L', 'M']
                .map((p) => DropdownMenuItem(value: p, child: Text(p == 'auto' ? 'Auto' : 'Preset $p', style: const TextStyle(fontSize: 13))))
                .toList(),
            onChanged: (v) => state.setTaskConfig(preset: v!),
          ),
        ),
        _buildFormRow(
          'Frame Gen',
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Switch(
                value: state.fgEnabled,
                onChanged: fgCanRun ? (v) => state.setTaskConfig(fgEnabled: v) : null,
              ),
              const SizedBox(width: 8),
              if (!fgCanRun)
                Expanded(
                  child: Text(
                    'Unavailable on this device',
                    style: TextStyle(fontSize: 12, color: Colors.orange.shade300),
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
                          label: Text('${multiplier}x', style: const TextStyle(fontSize: 12)),
                          selected: state.fgMultiplier == multiplier,
                          onSelected: (_) => state.setTaskConfig(fgMultiplier: multiplier),
                          visualDensity: VisualDensity.compact,
                          materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                        ),
                    ],
                  ),
                ),
            ],
          ),
        ),
        const Divider(height: 24, color: Color(0xFF333333)),
        _buildFormRow(
          'Format',
          DropdownButtonFormField<String>(
            initialValue: formats.map((f) => f.value).contains(state.outputFormat) ? state.outputFormat : formats.first.value,
            isExpanded: true,
            isDense: true,
            decoration: const InputDecoration(
              isDense: true,
              contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 8),
              border: OutlineInputBorder(),
            ),
            items: formats
                .map((f) => DropdownMenuItem(value: f.value, child: Text(f.label, style: const TextStyle(fontSize: 13))))
                .toList(),
              onChanged: (v) => state.setTaskConfig(outputFormat: v),
          ),
        ),
        if (state.outputFormat == 'png_16')
          _buildPercentSlider(
            'PNG Quality',
            state.pngQuality,
            (v) => state.setTaskConfig(pngQuality: v),
          ),
        if (state.outputFormat == 'exr') ...[
          _buildFormRow(
            'EXR Compression',
            DropdownButtonFormField<String>(
              initialValue: ExrCompression.values.map((c) => c.value).contains(state.exrCompression)
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
                  .map((c) => DropdownMenuItem(value: c.value, child: Text(c.label, style: const TextStyle(fontSize: 13))))
                  .toList(),
              onChanged: (v) => state.setTaskConfig(exrCompression: v),
            ),
          ),
          if (state.exrCompression != ExrCompression.none.value)
            _buildPercentSlider(
              'Compression',
              state.exrCompressionLevel,
              (v) => state.setTaskConfig(exrCompressionLevel: v),
            ),
        ],
        _buildFormRow(
          'Output Dir',
          Row(
            children: [
              Expanded(
                child: Container(
                  padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                  decoration: BoxDecoration(
                    border: Border.all(color: Colors.white24),
                    borderRadius: BorderRadius.circular(4),
                  ),
                  child: Text(
                    state.outputDir.isEmpty ? 'Select folder...' : state.outputDir,
                    style: TextStyle(
                      fontSize: 13,
                      fontFamily: 'monospace',
                      color: state.outputDir.isEmpty ? Colors.white38 : Colors.white70,
                    ),
                    overflow: TextOverflow.ellipsis,
                  ),
                ),
              ),
              const SizedBox(width: 8),
              IconButton.outlined(
                icon: const Icon(Icons.folder, size: 16),
                onPressed: () async {
                  final dir = selectDirectory();
                  if (dir != null) state.setTaskConfig(outputDir: dir);
                },
              ),
            ],
          ),
        ),
        _buildFormRow(
          'Template',
          Row(
            children: [
              Expanded(
                child: TextField(
                  controller: _templateController,
                  style: const TextStyle(fontSize: 13, fontFamily: 'monospace'),
                  decoration: const InputDecoration(
                    isDense: true,
                    contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                    border: OutlineInputBorder(),
                    hintText: 'frame_####',
                  ),
                ),
              ),
              const SizedBox(width: 12),
              Flexible(
                child: Text(
                  '→ ${_templateSample(state)}',
                  style: const TextStyle(fontSize: 12, color: Colors.white38, fontFamily: 'monospace'),
                  overflow: TextOverflow.ellipsis,
                ),
              ),
            ],
          ),
        ),
        _buildFormRow(
          'On Conflict',
          DropdownButtonFormField<String>(
            initialValue: state.overwriteMode,
            isExpanded: true,
            isDense: true,
            decoration: const InputDecoration(
              isDense: true,
              contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 8),
              border: OutlineInputBorder(),
            ),
            items: const [
              DropdownMenuItem(value: 'skip', child: Text('Skip existing', style: TextStyle(fontSize: 13))),
              DropdownMenuItem(value: 'overwrite', child: Text('Overwrite', style: TextStyle(fontSize: 13))),
              DropdownMenuItem(value: 'autoVersion', child: Text('Auto-version (_v##)', style: TextStyle(fontSize: 13))),
            ],
            onChanged: (v) => state.setTaskConfig(overwriteMode: v!),
          ),
        ),
      ],
    );
  }

  Widget _buildPercentSlider(String label, int value, ValueChanged<int> onChanged) {
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
              style: const TextStyle(color: Colors.white70, fontSize: 12, fontFamily: 'monospace'),
            ),
          ),
        ],
      ),
    );
  }
}
