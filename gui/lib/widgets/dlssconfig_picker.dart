import 'dart:convert';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../state/app_state.dart';
import '../util/error_dialog.dart';
import '../util/win_file_dialog.dart';
import '../util/app_log.dart';

/// dlssconfig.json file picker with schema validation and summary display.
class DlssConfigPicker extends StatelessWidget {
  const DlssConfigPicker({super.key});

  Future<void> _pickFile(BuildContext context) async {
    AppLog.phase('config', 'Opening dlssconfig picker');
    final path = selectFile(
      filterDescription: 'DLSS Config',
      filterPattern: '*.json',
    );
    if (path == null || !context.mounted) {
      AppLog.phase('config', 'dlssconfig picker cancelled');
      return;
    }

    AppLog.phase('config', 'dlssconfig selected', AppLog.summarizePath(path));

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

      AppLog.phase('config', 'dlssconfig parsed', {
        'valid': error == null,
        'error': error,
      });
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
      AppLog.phase('config', 'dlssconfig parse failed', e);
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
        return Container(
          decoration: BoxDecoration(
            color: Theme.of(context).colorScheme.surfaceContainerHighest.withValues(alpha: 0.3),
            border: Border.all(color: Theme.of(context).dividerColor.withValues(alpha: 0.1)),
            borderRadius: BorderRadius.circular(6),
          ),
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Wrap(
                crossAxisAlignment: WrapCrossAlignment.center,
                spacing: 12,
                runSpacing: 8,
                children: [
                  FilledButton.tonalIcon(
                    onPressed: () => _pickFile(context),
                    icon: const Icon(Icons.folder_open, size: 16),
                    label: const Text('Open dlssconfig.json'),
                    style: FilledButton.styleFrom(
                      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
                      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(6)),
                    ),
                  ),
                  if (state.currentDlssConfigPath != null)
                    ConstrainedBox(
                      constraints: const BoxConstraints(maxWidth: 360),
                      child: Tooltip(
                        message: state.currentDlssConfigPath!,
                        child: Text(
                          state.currentDlssConfigPath!,
                          style: TextStyle(
                            fontSize: 12,
                            color: Theme.of(context).colorScheme.onSurfaceVariant,
                            fontFamily: 'monospace',
                          ),
                          overflow: TextOverflow.ellipsis,
                        ),
                      ),
                    ),
                ],
              ),
              if (state.configError != null) ...[
                const SizedBox(height: 12),
                _errorBadge(state.configError!),
              ] else if (state.currentDlssConfig != null) ...[
                const SizedBox(height: 12),
                _buildSummary(state.currentDlssConfig!, context),
              ],
            ],
          ),
        );
      },
    );
  }

  Widget _buildSummary(Map<String, dynamic> config, BuildContext context) {
    final outRes = config['output_resolution'] as Map<String, dynamic>?;
    final renderRes = config['render_resolution'] as Map<String, dynamic>?;
    
    int? numFrames;
    final dynamic framesRaw = config['frames'];
    if (framesRaw is List) {
      numFrames = framesRaw.length;
    } else if (framesRaw is Map) {
      numFrames = framesRaw.length;
    }

    return Wrap(
      spacing: 8,
      runSpacing: 8,
      children: [
        _badge(context, Icons.check_circle, 'Valid', Colors.green),
        if (renderRes != null)
          _badge(context, Icons.input,
              'In: ${renderRes['width']}×${renderRes['height']}', Colors.grey),
        if (outRes != null)
          _badge(context, Icons.output,
              'Out: ${outRes['width']}×${outRes['height']}', Colors.blue),
        _badge(context, Icons.settings, '${config['quality_mode'] ?? 'N/A'}', Colors.orange),
        if (config['is_hdr'] == true) _badge(context, Icons.hdr_on, 'HDR', Colors.purple),
        _badge(context, Icons.speed, '${config['fps'] ?? 0} fps', Colors.teal),
        if (numFrames != null)
          _badge(context, Icons.movie, '$numFrames frames', Colors.indigo),
      ],
    );
  }

  Widget _badge(BuildContext context, IconData icon, String label, MaterialColor color) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.1),
        border: Border.all(color: color.withValues(alpha: 0.2)),
        borderRadius: BorderRadius.circular(4),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(icon, size: 12, color: color.shade400),
          const SizedBox(width: 6),
          Text(
            label,
            style: TextStyle(
              fontSize: 11,
              fontWeight: FontWeight.w500,
              color: color.shade200,
            ),
          ),
        ],
      ),
    );
  }

  Widget _errorBadge(String message) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
      decoration: BoxDecoration(
        color: Colors.red.withValues(alpha: 0.12),
        border: Border.all(color: Colors.red.withValues(alpha: 0.3)),
        borderRadius: BorderRadius.circular(6),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          const Icon(Icons.error_outline, size: 14, color: Colors.redAccent),
          const SizedBox(width: 8),
          Text(
            message,
            style: const TextStyle(fontSize: 12, color: Colors.redAccent),
          ),
        ],
      ),
    );
  }
}
