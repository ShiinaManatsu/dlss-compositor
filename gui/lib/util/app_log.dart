import 'dart:io';
import 'package:flutter/foundation.dart';

class AppLog {
  static const String _fileName = 'dlss-compositor-gui.log';
  static const bool _enabledByBuild = bool.fromEnvironment(
    'DLSS_COMPOSITOR_GUI_LOG',
    defaultValue: false,
  );
  static File? _file;
  static bool _disabled = false;

  static void resetStartup() {
    if (!_shouldLog) {
      _disabled = true;
      return;
    }
    final file = _resolveLogFile();
    _file = file;
    if (!file.existsSync()) {
      try {
        file.writeAsStringSync('');
      } on FileSystemException {
        _disabled = true;
        return;
      }
    }
    phase('startup', 'Dart startup logger ready');
  }

  static void phase(String stage, String message, [Object? details]) {
    if (_disabled || !_shouldLog) return;
    final file = _file ??= _resolveLogFile();
    final suffix = details == null ? '' : ' | $details';
    final line = '[${DateTime.now().toIso8601String()}] [$stage] $message$suffix';
    try {
      file.writeAsStringSync('$line\n', mode: FileMode.append);
    } on FileSystemException {
      _disabled = true;
    }
  }

  static Map<String, Object?> summarizeTaskParams(Map<String, dynamic> params) {
    return {
      'preset': params['preset'],
      'fg_enabled': params['fg_enabled'],
      'fg_multiplier': params['fg_multiplier'],
      'output_format': params['output_format'],
      'writer_threads': params['writer_threads'],
      'writer_buffer_multiplier': params['writer_buffer_multiplier'],
      'overwrite_mode': params['overwrite_mode'],
    };
  }

  static Map<String, Object?> summarizePath(String path) {
    final normalized = path.replaceAll('\\', '/');
    final slash = normalized.lastIndexOf('/');
    return {'file': slash >= 0 ? normalized.substring(slash + 1) : normalized};
  }

  static File _resolveLogFile() {
    final exeDir = File(Platform.resolvedExecutable).parent;
    final logDir = Directory('${exeDir.path}${Platform.pathSeparator}logs');
    if (!logDir.existsSync()) {
      logDir.createSync(recursive: true);
    }
    return File('${logDir.path}${Platform.pathSeparator}$_fileName');
  }

  static bool get _shouldLog =>
      kDebugMode || _enabledByBuild || Platform.environment['DLSS_COMPOSITOR_GUI_LOG'] == '1';

}
