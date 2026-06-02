import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../state/app_state.dart';
import '../plugin/dlss_compositor.dart';
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
  // Configuration state
  String _preset = 'auto';
  bool _fgEnabled = false;
  FgMultiplier _fgMultiplier = FgMultiplier.two;
  OutputFormat? _outputFormat;
  OverwriteMode _overwriteMode = OverwriteMode.overwrite;
  String _outputDir = '';
  final _templateController = TextEditingController(text: 'frame_####');

  @override
  void initState() {
    super.initState();
    _templateController.addListener(_onTemplateChanged);
  }

  @override
  void dispose() {
    _templateController.dispose();
    super.dispose();
  }

  void _onTemplateChanged() {
    setState(() {});
  }

  String get _templateSample {
    final tmpl = _templateController.text;
    final formatted = tmpl.replaceAll('####', '0001');
    final ext = _outputFormat == OutputFormat.png16 ? '.png'
              : _outputFormat == OutputFormat.prores422hq ? '.mov'
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
    if (appState.currentDlssConfig == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Please select a valid dlssconfig.json first')));
      return;
    }

    // Check output collisions before starting
    // ignore: unused_local_variable
    final collisions = await DlssCompositor.getProgress();  // placeholder
    // TODO: call checkOutputCollisions RPC when integrated

    // For now: proceed directly
    await DlssCompositor.setTask({
      'config': appState.currentDlssConfigPath,
      'preset': _preset,
      'fg_enabled': _fgEnabled,
      'fg_multiplier': _fgMultiplier.index + 2,
      'output_format': _outputFormat?.value ?? 'exr',
      'output_dir': _outputDir,
      'output_template': _templateController.text,
      'overwrite_mode': _overwriteMode.name,
    });
    await DlssCompositor.startTask();
    appState.setTaskState(TaskState.running);
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<AppState>(
      builder: (context, state, _) {
        final isHdr = state.currentDlssConfig?['is_hdr'] as bool? ?? false;
        final availableFormats = _getAvailableFormats(isHdr);

        // Reset format if not available for current HDR/LDR mode
        if (_outputFormat != null && !availableFormats.contains(_outputFormat)) {
          // Use Future.microtask to avoid setState during build
          Future.microtask(() {
              if (mounted) {
                setState(() {
                  _outputFormat = availableFormats.first;
                });
              }
          });
        }
        _outputFormat ??= availableFormats.first;

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
              _buildSection('Preset', _buildPresetSelector()),
              const SizedBox(height: 8),

              // 4. FG settings
              _buildSection('Frame Generation', _buildFgSettings(state.fgMaxMultiplier)),
              const SizedBox(height: 8),

              // 5. Output settings
              _buildSection('Output', _buildOutputSettings(isHdr, availableFormats)),
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

  Widget _buildPresetSelector() {
    return DropdownButton<String>(
      value: _preset,
      isDense: true,
      items: ['auto', 'J', 'K', 'L', 'M']
          .map((p) => DropdownMenuItem(value: p, child: Text(p == 'auto' ? 'Auto' : 'Preset $p')))
          .toList(),
      onChanged: (v) => setState(() => _preset = v!),
    );
  }

  Widget _buildFgSettings(int fgMax) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            const Text('Enable FG:', style: TextStyle(fontSize: 13)),
            Switch(
              value: _fgEnabled,
              onChanged: (v) => setState(() => _fgEnabled = v),
            ),
          ],
        ),
        if (_fgEnabled) ...[
          const Text('Multiplier:', style: TextStyle(fontSize: 12)),
          Row(
            children: [
              for (final m in FgMultiplier.values)
                Padding(
                  padding: const EdgeInsets.only(right: 4),
                  child: ChoiceChip(
                    label: Text('${m.index + 2}x'),
                    selected: _fgMultiplier == m,
                    // Gray out multipliers exceeding hardware max
                    onSelected: (m.index + 2 <= fgMax)
                        ? (_) => setState(() => _fgMultiplier = m)
                        : null,
                  ),
                ),
            ],
          ),
        ],
      ],
    );
  }

  Widget _buildOutputSettings(bool isHdr, List<OutputFormat> formats) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        // Output format dropdown — ONLY shows v2 implemented formats.
        // NOTE: Additional formats reserved for future releases - see docs/cli_reference.md.
        Row(
          children: [
            const Text('Format:', style: TextStyle(fontSize: 12)),
            const SizedBox(width: 8),
            DropdownButton<OutputFormat>(
              value: _outputFormat,
              isDense: true,
              items: formats
                  .map((f) => DropdownMenuItem(value: f, child: Text(f.label)))
                  .toList(),
                onChanged: (v) => setState(() {
                  _outputFormat = v;
                }),
            ),
          ],
        ),
        const SizedBox(height: 8),

        // Output directory
        Row(
          children: [
            const Text('Output Dir:', style: TextStyle(fontSize: 12)),
            const SizedBox(width: 8),
            Expanded(
              child: Text(_outputDir.isEmpty ? '(not set)' : _outputDir,
                  style: TextStyle(
                      fontSize: 12,
                      color: _outputDir.isEmpty ? Colors.grey : null),
                  overflow: TextOverflow.ellipsis),
            ),
            IconButton(
              icon: const Icon(Icons.folder, size: 16),
              onPressed: () async {
                final dir = selectDirectory();
                if (dir != null) setState(() => _outputDir = dir);
              },
            ),
          ],
        ),

        // Filename template
        const SizedBox(height: 8),
        Row(
          children: [
            const Text('Template:', style: TextStyle(fontSize: 12)),
            const SizedBox(width: 8),
            SizedBox(
              width: 140,
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
            Text('→ $_templateSample', style: const TextStyle(fontSize: 11, color: Colors.grey)),
          ],
        ),

        // Overwrite mode
        const SizedBox(height: 8),
        Row(
          children: [
            const Text('On conflict:', style: TextStyle(fontSize: 12)),
            const SizedBox(width: 8),
            DropdownButton<OverwriteMode>(
              value: _overwriteMode,
              isDense: true,
              items: const [
                DropdownMenuItem(value: OverwriteMode.skip,
                    child: Text('Skip existing')),
                DropdownMenuItem(value: OverwriteMode.overwrite,
                    child: Text('Overwrite')),
                DropdownMenuItem(value: OverwriteMode.autoVersion,
                    child: Text('Auto-version (_v##)')),
              ],
              onChanged: (v) => setState(() => _overwriteMode = v!),
            ),
          ],
        ),
      ],
    );
  }
}
