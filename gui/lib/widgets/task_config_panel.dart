import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../state/app_state.dart';
import '../state/preferences.dart';
import '../plugin/dlss_compositor.dart';
import '../util/error_dialog.dart';
import '../util/win_file_dialog.dart';
import 'dlssconfig_picker.dart';

enum OverwriteMode { skip, overwrite, autoVersion }
enum OutputFormat { png16, exrLinear, prores422hq }
enum FgMultiplier { two, three, four }

extension OutputFormatExt on OutputFormat {
  String get label {
    switch (this) {
      case OutputFormat.png16:       return 'PNG (16-bit)';
      case OutputFormat.exrLinear:   return 'EXR (scene-linear)';
      case OutputFormat.prores422hq: return 'ProRes 422 HQ';
    }
  }
  String get value {
    switch (this) {
      case OutputFormat.png16:       return 'png_16';
      case OutputFormat.exrLinear:   return 'exr';
      case OutputFormat.prores422hq: return 'prores_422hq';
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
    if (_templateController.text != state.template) {
      _templateController.text = state.template;
    }
  }

  @override
  void dispose() {
    _templateController.dispose();
    super.dispose();
  }

  void _onTemplateChanged() {
    final state = Provider.of<AppState>(context, listen: false);
    if (state.template != _templateController.text) {
      state.setTaskConfig(template: _templateController.text);
    }
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
          prefs.outputDirectory != state.outputDir) {
        Future.microtask(() {
          if (!mounted) return;
          state.setTaskConfig(
            preset: prefs.presetOverride,
            fgEnabled: prefs.fgEnabled,
            fgMultiplier: prefs.fgMultiplier,
            outputFormat: prefs.outputFormat,
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
    final tmpl = state.template;
    final formatted = tmpl.replaceAll('####', '0001');
    final ext = state.outputFormat == 'png_16' ? '.png'
              : state.outputFormat == 'prores_422hq' ? '.mov'
              : '.exr';
    return formatted.contains('.') ? formatted : '$formatted$ext';
  }

  List<OutputFormat> _getAvailableFormats(bool isHdr) {
    if (isHdr) {
      // HDR: EXR + ProRes only (NO PNG for HDR projects)
      return [OutputFormat.exrLinear, OutputFormat.prores422hq];
    } else {
      // LDR: PNG + EXR + ProRes (LDR gets PNG per user round 2 decision)
      return [OutputFormat.png16, OutputFormat.exrLinear, OutputFormat.prores422hq];
    }
    // NOTE: Additional formats reserved for future releases - see docs/cli_reference.md.
  }

  Future<void> _onStartPressed(BuildContext context) async {
    final appState = Provider.of<AppState>(context, listen: false);
    final prefs = Provider.of<AppPreferences>(context, listen: false);
    if (appState.currentDlssConfig == null) {
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
      await showErrorDialog(
        context,
        title: 'Invalid filename template',
        message: templateError,
      );
      return;
    }

    final outputDir = appState.outputDir.trim();
    if (outputDir.isEmpty) {
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
      'fg_enabled': appState.fgEnabled,
      'fg_multiplier': appState.fgMultiplier,
      'output_format': appState.outputFormat,
      'output_dir': outputDir,
      'output_template': appState.template.trim(),
      'overwrite_mode': appState.overwriteMode,
    };

    final collisions = await DlssCompositor.checkOutputCollisions(taskParams);
    final count = collisions['collisions'] as int? ?? 0;
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
      if (proceed != true) return;
    }

    prefs.setPresetOverride(appState.preset);
    prefs.setFgEnabled(appState.fgEnabled);
    prefs.setFgMultiplier(appState.fgMultiplier);
    prefs.setOutputFormat(appState.outputFormat);
    prefs.setOutputDirectory(outputDir);

    final setOk = await DlssCompositor.setTask(taskParams);
    if (!setOk && context.mounted) {
      await showErrorDialog(
        context,
        title: 'Backend rejected task',
        message: 'Failed to set task parameters.',
        details: taskParams.toString(),
      );
      return;
    }

    final startOk = await DlssCompositor.startTask();
    if (startOk) {
      // Reset state for new task
      appState.updateProgress(0, 0, 0.0);
      appState.setTaskState(TaskState.running, error: null);
    } else if (context.mounted) {
      await showErrorDialog(
        context,
        title: 'Backend failed to start',
        message: 'Failed to start the task.',
        details: taskParams.toString(),
      );
    }
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
              const SizedBox(height: 12),

              // 2. Readonly config info
              if (state.currentDlssConfig != null) _buildConfigInfo(state.currentDlssConfig!),

              const SizedBox(height: 12),

              // 3. Preset selector
              _buildSection('Preset', _buildPresetSelector(state)),
              const SizedBox(height: 8),

              // 4. FG settings
              _buildSection('Frame Generation', _buildFgSettings(state)),
              const SizedBox(height: 8),

              // 5. Output settings
              _buildSection('Output', _buildOutputSettings(state, availableFormats)),
              const SizedBox(height: 16),

              // 6. Start button
              ElevatedButton.icon(
                onPressed: state.currentDlssConfig != null &&
                           state.taskState != TaskState.running
                    ? () => _onStartPressed(context)
                    : null,
                icon: const Icon(Icons.play_arrow),
                label: Text(state.taskState == TaskState.running
                    ? 'Processing...' : 'Start'),
                style: ElevatedButton.styleFrom(
                  padding: const EdgeInsets.symmetric(vertical: 12),
                  backgroundColor: Colors.green,
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
        Text(title, style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 13)),
        const SizedBox(height: 4),
        child,
      ],
    );
  }

  Widget _buildConfigInfo(Map<String, dynamic> config) {
    final outRes = config['output_resolution'] as Map<String, dynamic>?;
    final renderRes = config['render_resolution'] as Map<String, dynamic>?;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(8),
        child: Wrap(
          spacing: 8,
          runSpacing: 4,
          children: [
            if (outRes != null) Text('Output: ${outRes['width']}×${outRes['height']}',
                style: const TextStyle(fontSize: 12)),
            if (renderRes != null) Text('Render: ${renderRes['width']}×${renderRes['height']}',
                style: const TextStyle(fontSize: 12)),
            Text('Quality: ${config['quality_mode'] ?? 'N/A'}',
                style: const TextStyle(fontSize: 12)),
            Text('FPS: ${config['fps'] ?? 0}',
                style: const TextStyle(fontSize: 12)),
          ],
        ),
      ),
    );
  }

  Widget _buildPresetSelector(AppState state) {
    return DropdownButton<String>(
      value: state.preset,
      isDense: true,
      items: ['auto', 'J', 'K', 'L', 'M']
          .map((p) => DropdownMenuItem(value: p, child: Text(p == 'auto' ? 'Auto' : 'Preset $p')))
          .toList(),
      onChanged: (v) => state.setTaskConfig(preset: v!),
    );
  }

  Widget _buildFgSettings(AppState state) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            const Text('Enable FG:', style: TextStyle(fontSize: 13)),
            Switch(
              value: state.fgEnabled,
              onChanged: (v) => state.setTaskConfig(fgEnabled: v),
            ),
          ],
        ),
        if (state.fgEnabled) ...[
          const Text('Multiplier:', style: TextStyle(fontSize: 12)),
          Row(
            children: [
              for (final m in FgMultiplier.values)
                Padding(
                  padding: const EdgeInsets.only(right: 4),
                  child: ChoiceChip(
                    label: Text('${m.index + 2}x'),
                    selected: state.fgMultiplier == (m.index + 2),
                    // Gray out multipliers exceeding hardware max
                    onSelected: (m.index + 2 <= state.fgMaxMultiplier)
                        ? (_) => state.setTaskConfig(fgMultiplier: m.index + 2)
                        : null,
                  ),
                ),
            ],
          ),
        ],
      ],
    );
  }

  Widget _buildOutputSettings(AppState state, List<OutputFormat> formats) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        // Output format dropdown — ONLY shows v2 implemented formats.
        // NOTE: Additional formats reserved for future releases - see docs/cli_reference.md.
        Row(
          children: [
            const SizedBox(
              width: 84,
              child: Text('Format:', style: TextStyle(fontSize: 12)),
            ),
            Expanded(
              child: DropdownButton<String>(
                value: formats.map((f) => f.value).contains(state.outputFormat) ? state.outputFormat : formats.first.value,
                isExpanded: true,
                isDense: true,
                items: formats
                    .map((f) => DropdownMenuItem(value: f.value, child: Text(f.label)))
                    .toList(),
                  onChanged: (v) => state.setTaskConfig(outputFormat: v),
              ),
            ),
          ],
        ),
        const SizedBox(height: 8),

        // Output directory
        Row(
          children: [
            const SizedBox(
              width: 84,
              child: Text('Output Dir:', style: TextStyle(fontSize: 12)),
            ),
            Expanded(
              child: Text(state.outputDir.isEmpty ? '(not set)' : state.outputDir,
                  style: TextStyle(
                      fontSize: 12,
                      color: state.outputDir.isEmpty ? Colors.grey : null),
                  overflow: TextOverflow.ellipsis),
            ),
            IconButton(
              icon: const Icon(Icons.folder, size: 16),
              onPressed: () async {
                final dir = selectDirectory();
                if (dir != null) state.setTaskConfig(outputDir: dir);
              },
            ),
          ],
        ),

        // Filename template
        const SizedBox(height: 8),
        Row(
          children: [
            const SizedBox(
              width: 84,
              child: Text('Template:', style: TextStyle(fontSize: 12)),
            ),
            Expanded(
              child: TextField(
                controller: _templateController,
                style: const TextStyle(fontSize: 12),
                decoration: const InputDecoration(
                  isDense: true,
                  border: OutlineInputBorder(),
                  hintText: 'frame_####',
                ),
              ),
            ),
            const SizedBox(width: 8),
            Flexible(
              child: Text(
                '→ ${_templateSample(state)}',
                style: const TextStyle(fontSize: 11, color: Colors.grey),
                overflow: TextOverflow.ellipsis,
              ),
            ),
          ],
        ),

        // Overwrite mode
        const SizedBox(height: 8),
        Row(
          children: [
            const SizedBox(
              width: 84,
              child: Text('On conflict:', style: TextStyle(fontSize: 12)),
            ),
            Expanded(
              child: DropdownButton<String>(
                value: state.overwriteMode,
                isExpanded: true,
                isDense: true,
                items: const [
                  DropdownMenuItem(value: 'skip',
                      child: Text('Skip existing')),
                  DropdownMenuItem(value: 'overwrite',
                      child: Text('Overwrite')),
                  DropdownMenuItem(value: 'autoVersion',
                      child: Text('Auto-version (_v##)')),
                ],
                onChanged: (v) => state.setTaskConfig(overwriteMode: v!),
              ),
            ),
          ],
        ),
      ],
    );
  }
}
