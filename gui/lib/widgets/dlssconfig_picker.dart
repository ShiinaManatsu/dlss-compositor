import 'dart:convert';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../state/app_state.dart';
import '../util/error_dialog.dart';
import '../util/win_file_dialog.dart';

/// dlssconfig.json file picker with schema validation and summary display.
class DlssConfigPicker extends StatelessWidget {
  const DlssConfigPicker({super.key});

  Future<void> _pickFile(BuildContext context) async {
    final path = selectFile(
      filterDescription: 'DLSS Config',
      filterPattern: '*.json',
    );
    if (path == null || !context.mounted) return;

    final appState = Provider.of<AppState>(context, listen: false);

    // Load and validate the selected file
    try {
      final content = await File(path).readAsString();
      final config = json.decode(content) as Map<String, dynamic>;

      // Basic validation
      String? error;
      if (config['version'] != 1) {
        error = 'Invalid version (expected 1, got ${config['version']})';
      } else if (!config.containsKey('output_resolution')) {
        error = 'Missing required field: output_resolution';
      } else if (!config.containsKey('fps')) {
        error = 'Missing required field: fps';
      }

      appState.setDlssConfig(path, error == null ? config : null, error);
      if (error != null && context.mounted) {
        await showErrorDialog(
          context,
          title: 'Invalid dlssconfig.json',
          message: error,
          details: path,
        );
      }
    } catch (e) {
      appState.setDlssConfig(path, null, 'Failed to parse dlssconfig.json');
      if (context.mounted) {
        await showErrorDialog(
          context,
          title: 'Failed to open dlssconfig.json',
          message: 'The selected file could not be parsed.',
          details: '$path\n$e',
        );
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<AppState>(
      builder: (context, state, _) {
        return Card(
          child: Padding(
            padding: const EdgeInsets.all(12),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Wrap(
                  spacing: 12,
                  runSpacing: 8,
                  crossAxisAlignment: WrapCrossAlignment.center,
                  children: [
                    ElevatedButton.icon(
                      onPressed: () => _pickFile(context),
                      icon: const Icon(Icons.folder_open, size: 16),
                      label: const Text('Open dlssconfig.json'),
                    ),
                    if (state.currentDlssConfigPath != null)
                      ConstrainedBox(
                        constraints: const BoxConstraints(maxWidth: 520),
                        child: Text(
                          state.currentDlssConfigPath!,
                          style: const TextStyle(fontSize: 12),
                          overflow: TextOverflow.ellipsis,
                        ),
                      ),
                  ],
                ),

                if (state.currentDlssConfig != null) ...[
                  const SizedBox(height: 8),
                  _buildSummary(state.currentDlssConfig!),
                ],
              ],
            ),
          ),
        );
      },
    );
  }

  Widget _buildSummary(Map<String, dynamic> config) {
    final outRes = config['output_resolution'] as Map<String, dynamic>?;
    
    int? numFrames;
    final dynamic framesRaw = config['frames'];
    if (framesRaw is List) {
      numFrames = framesRaw.length;
    } else if (framesRaw is Map) {
      numFrames = framesRaw.length;
    }

    return Wrap(
      spacing: 16,
      children: [
        _chip(Icons.check_circle, 'Valid', Colors.green),
        if (outRes != null)
          _chip(Icons.photo_size_select_actual,
              '${outRes['width']}×${outRes['height']}', Colors.blue),
        _chip(Icons.settings, '${config['quality_mode'] ?? 'N/A'}', Colors.orange),
        if (config['is_hdr'] == true) _chip(Icons.hdr_on, 'HDR', Colors.purple),
        _chip(Icons.speed, '${config['fps'] ?? 0} fps', Colors.teal),
        if (numFrames != null)
          _chip(Icons.movie, '$numFrames frames', Colors.indigo),
      ],
    );
  }

  Widget _chip(IconData icon, String label, Color color) {
    return Chip(
      avatar: Icon(icon, size: 14, color: color),
      label: Text(label, style: const TextStyle(fontSize: 11)),
      materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
      padding: const EdgeInsets.symmetric(horizontal: 4),
      visualDensity: VisualDensity.compact,
    );
  }
}
