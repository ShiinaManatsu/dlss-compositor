import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../l10n/app_strings.dart';
import '../state/app_state.dart';
import '../util/app_log.dart';
import '../util/dlss_config_file.dart';
import '../util/error_dialog.dart';
import '../util/win_file_dialog.dart';

/// DLSS config file picker with schema validation and summary display.
class DlssConfigPicker extends StatelessWidget {
  const DlssConfigPicker({super.key});

  Future<void> _pickFile(BuildContext context) async {
    final strings = context.strings;
    AppLog.phase('config', 'Opening dlssconfig picker');
    final path = selectFile(
      filterDescription: kDlssConfigFileDescription,
      filterPattern: kDlssConfigFileFilter,
      title: strings.openDlssConfigDialogTitle,
    );
    if (path == null || !context.mounted) {
      AppLog.phase('config', 'dlssconfig picker cancelled');
      return;
    }

    AppLog.phase('config', 'dlssconfig selected', AppLog.summarizePath(path));

    final appState = Provider.of<AppState>(context, listen: false);

    try {
      final result = await loadDlssConfigFile(
        path,
        invalidVersion: strings.invalidVersion,
        missingRequiredField: strings.missingRequiredField,
      );
      AppLog.phase('config', 'dlssconfig parsed', {
        'valid': result.isValid,
        'error': result.error,
      });
      appState.setDlssConfig(path, result.config, result.error);
      if (result.error != null && context.mounted) {
        await showErrorDialog(
          context,
          title: strings.invalidConfigTitle,
          message: result.error!,
          details: path,
        );
      }
    } catch (e) {
      AppLog.phase('config', 'dlssconfig parse failed', e);
      appState.setDlssConfig(path, null, strings.failedOpenConfigMessage);
      if (context.mounted) {
        await showErrorDialog(
          context,
          title: strings.failedOpenConfigTitle,
          message: strings.failedOpenConfigMessage,
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
                    label: Text(context.strings.openDlssConfig),
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
    final strings = context.strings;
    final outRes = config['output_resolution'] as Map<String, dynamic>?;
    final renderRes = config['render_resolution'] as Map<String, dynamic>?;

    int? numFrames;
    final framesRaw = config['frames'];
    if (framesRaw is List) {
      numFrames = framesRaw.length;
    } else if (framesRaw is Map) {
      numFrames = framesRaw.length;
    }

    return Wrap(
      spacing: 8,
      runSpacing: 8,
      children: [
        _badge(context, Icons.check_circle, strings.valid, Colors.green),
        if (renderRes != null)
          _badge(
            context,
            Icons.input,
            strings.inputResolution(renderRes['width'], renderRes['height']),
            Colors.grey,
          ),
        if (outRes != null)
          _badge(
            context,
            Icons.output,
            strings.outputResolution(outRes['width'], outRes['height']),
            Colors.blue,
          ),
        _badge(
          context,
          Icons.settings,
          '${config['quality_mode'] ?? strings.qualityFallback}',
          Colors.orange,
        ),
        if (config['is_hdr'] == true)
          _badge(context, Icons.hdr_on, strings.hdr, Colors.purple),
        _badge(
          context,
          Icons.speed,
          strings.fps(config['fps'] as int? ?? 0),
          Colors.teal,
        ),
        if (numFrames != null)
          _badge(context, Icons.movie, strings.frames(numFrames), Colors.indigo),
      ],
    );
  }

  Widget _badge(
    BuildContext context,
    IconData icon,
    String label,
    MaterialColor color,
  ) {
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
