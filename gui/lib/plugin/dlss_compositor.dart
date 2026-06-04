import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:dlss_compositor_plugin/dlss_compositor_plugin.dart';

import '../util/app_log.dart';

/// Dart wrapper for DLSS Compositor native plugin.
///
/// Communicates via MethodChannel (commands), EventChannel (events), and FFI
/// pointers for zero-copy preview frame reads.
class DlssCompositor {
  static final DlssCompositorPlugin _plugin = DlssCompositorPlugin();
  static final StreamController<Map<String, dynamic>> _events =
      StreamController<Map<String, dynamic>>.broadcast();
  static StreamSubscription<Map<String, dynamic>>? _nativeEvents;
  static Map<String, dynamic>? _taskParams;
  static Process? _runningProcess;

  /// Current processing path launches the CLI as a child process. The native
  /// plugin reads preview frames from a named shared memory block populated by
  /// the CLI, so live preview is available even with the external backend.
  static bool get supportsLivePreview => true;

  /// Send a task configuration to the native engine.
  static Future<bool> setTask(Map<String, dynamic> params) async {
    AppLog.phase('plugin', 'setTask begin', AppLog.summarizeTaskParams(params));
    _taskParams = Map<String, dynamic>.from(params);
    final result = await _plugin.setTask(params);
    AppLog.phase('plugin', 'setTask complete', {'ok': result});
    return result;
  }

  /// Check if the engine is ready to process tasks.
  static Future<bool> checkReady() async {
    AppLog.phase('plugin', 'checkReady begin');
    final result = await _plugin.checkReady();
    AppLog.phase('plugin', 'checkReady complete', {'ready': result});
    return result;
  }

  /// Get DLSS status (version, GPU, SR/FG availability).
  static Future<Map<String, dynamic>> getStatus() async {
    AppLog.phase('plugin', 'getStatus begin');
    final result = await _plugin.getStatus();
    AppLog.phase('plugin', 'getStatus complete', result);
    return result;
  }

  /// Get current render progress.
  static Future<Map<String, dynamic>> getProgress() async {
    AppLog.phase('plugin', 'getProgress begin');
    final result = await _plugin.getProgress();
    AppLog.phase('plugin', 'getProgress complete', result);
    return result;
  }

  /// Start the render task.
  static Future<bool> startTask() async {
    AppLog.phase('plugin', 'startTask begin');
    if (_runningProcess != null) {
      AppLog.phase('plugin', 'startTask rejected: already running');
      return false;
    }

    final params = _taskParams;
    if (params == null) {
      AppLog.phase('plugin', 'startTask rejected: missing task params');
      return false;
    }

    final cli = _findCliExecutable();
    if (cli == null) {
      AppLog.phase('plugin', 'startTask rejected: cli not found');
      return false;
    }

    final args = _buildCliArgs(params);
    args.addAll(['--preview-shm', _previewSharedMemoryName]);
    AppLog.phase('plugin', 'startTask spawning cli', {'exe': cli.path, 'args': args});
    try {
      final process = await Process.start(cli.path, args);
      _runningProcess = process;
      _wireProcessOutput(process);
      unawaited(process.exitCode.then((code) {
        _runningProcess = null;
        if (code == 0) {
          AppLog.phase('plugin', 'cli process complete');
          _events.add({'type': 'done'});
        } else {
          final message = 'CLI exited with code $code';
          AppLog.phase('plugin', 'cli process failed', message);
          _events.add({'type': 'error', 'message': message});
        }
      }));
      AppLog.phase('plugin', 'startTask complete', {'ok': true});
      return true;
    } on ProcessException catch (e) {
      AppLog.phase('plugin', 'startTask spawn failed', e.message);
      return false;
    }
  }

  /// Preflight output path collisions for the current task settings.
  static Future<Map<String, dynamic>> checkOutputCollisions(
    Map<String, dynamic> params,
  ) async {
    AppLog.phase('plugin', 'checkOutputCollisions begin', AppLog.summarizeTaskParams(params));
    final result = _checkOutputCollisionsLocally(params);
    AppLog.phase('plugin', 'checkOutputCollisions complete', result);
    return result;
  }

  /// Get latest preview frame via FFI shared memory (zero-copy).
  static Future<PreviewFrame?> getPreviewFrame() async {
    final frame = await _plugin.getPreviewFrame();
    if (frame != null) {
      AppLog.phase('preview', 'getPreviewFrame complete', {
        'width': frame.width,
        'height': frame.height,
        'bytes': frame.data.length,
      });
    }
    return frame;
  }

  /// Stream of progress and event updates from the native engine.
  static Stream<Map<String, dynamic>> get progressStream {
    AppLog.phase('plugin', 'progressStream listen requested');
    _ensureNativeEventsForwarded();
    return _events.stream.map((event) {
      AppLog.phase('event', 'progressStream event', event);
      return event;
    });
  }

  static void _ensureNativeEventsForwarded() {
    _nativeEvents ??= _plugin.progressStream.listen(_events.add, onError: _events.addError);
  }

  static List<String> _buildCliArgs(Map<String, dynamic> params) {
    final args = <String>['--config', params['config'] as String? ?? ''];
    final outputDir = params['output_dir'] as String? ?? '';
    if (outputDir.isNotEmpty) args.addAll(['--output-dir', outputDir]);
    final template = (params['template'] ?? params['output_template']) as String? ?? '';
    if (template.isNotEmpty) args.addAll(['--output-template', template]);
    final format = params['output_format'] as String? ?? 'exr';
    args.addAll(['--output-format', format]);
    final overwrite = params['overwrite_mode'] as String? ?? 'overwrite';
    args.addAll(['--overwrite-mode', overwrite]);
    args.addAll(['--events', 'stderr']);
    if (params['fg_enabled'] == true) {
      args.addAll(['--fg', '${params['fg_multiplier'] ?? 2}x']);
    }
    return args;
  }

  static File? _findCliExecutable() {
    final exeName = Platform.isWindows ? 'dlss-compositor-cli.exe' : 'dlss-compositor-cli';
    final roots = <Directory>[
      Directory.current,
      File(Platform.resolvedExecutable).parent,
    ];
    final projectRoot = _findProjectRoot(Directory.current) ??
        _findProjectRoot(File(Platform.resolvedExecutable).parent);
    if (projectRoot != null) roots.insert(0, projectRoot);

    for (final root in roots) {
      final candidates = [
        File('${root.path}${Platform.pathSeparator}$exeName'),
        File('${root.path}${Platform.pathSeparator}build${Platform.pathSeparator}Release${Platform.pathSeparator}$exeName'),
        File('${root.path}${Platform.pathSeparator}build${Platform.pathSeparator}Debug${Platform.pathSeparator}$exeName'),
        File('${root.path}${Platform.pathSeparator}build${Platform.pathSeparator}RelWithDebInfo${Platform.pathSeparator}$exeName'),
        File('${root.path}${Platform.pathSeparator}build${Platform.pathSeparator}x64-windows-msvc-release${Platform.pathSeparator}Release${Platform.pathSeparator}$exeName'),
        File('${root.path}${Platform.pathSeparator}build${Platform.pathSeparator}x64-windows-msvc-release${Platform.pathSeparator}Debug${Platform.pathSeparator}$exeName'),
      ];
      for (final candidate in candidates) {
        if (candidate.existsSync()) return candidate;
      }
    }
    return null;
  }

  static String get _previewSharedMemoryName {
    final raw = Directory.current.absolute.path
        .replaceAll(RegExp(r'[^A-Za-z0-9_]'), '_');
    return 'Local\\DLSSCompositorPreview_$raw';
  }

  static Directory? _findProjectRoot(Directory start) {
    var dir = start.absolute;
    while (true) {
      if (File('${dir.path}${Platform.pathSeparator}AGENTS.md').existsSync() &&
          File('${dir.path}${Platform.pathSeparator}CMakeLists.txt').existsSync()) {
        return dir;
      }
      final parent = dir.parent;
      if (parent.path == dir.path) return null;
      dir = parent;
    }
  }

  static void _wireProcessOutput(Process process) {
    void handleLine(String line) {
      AppLog.phase('cli', line);
      const prefix = 'DLSS_EVENT ';
      if (!line.startsWith(prefix)) return;
      final payload = jsonDecode(line.substring(prefix.length));
      if (payload is Map) {
        _events.add(Map<String, dynamic>.from(payload));
      }
    }

    process.stdout.transform(utf8.decoder).transform(const LineSplitter()).listen(handleLine);
    process.stderr.transform(utf8.decoder).transform(const LineSplitter()).listen(handleLine);
  }

  static Map<String, dynamic> _checkOutputCollisionsLocally(Map<String, dynamic> params) {
    final outputDir = params['output_dir'] as String? ?? '';
    final template = (params['template'] ?? params['output_template']) as String? ?? 'frame_####.exr';
    final frameCount = params['frame_count'] as int? ?? 0;
    if (outputDir.isEmpty || frameCount <= 0) {
      return {'collisions': 0, 'files': <String>[]};
    }
    final files = <String>[];
    for (var frame = 1; frame <= frameCount; frame++) {
      final path = '${Directory(outputDir).path}${Platform.pathSeparator}${_formatTemplate(template, frame)}';
      if (File(path).existsSync()) files.add(path);
    }
    return {'collisions': files.length, 'files': files};
  }

  static String _formatTemplate(String template, int frame) {
    final padded = frame.toString().padLeft(4, '0');
    if (template.contains('####')) return template.replaceAll('####', padded);
    return template.replaceAll(RegExp(r'\{frame:0?4d\}'), padded);
  }

  /// Set debug mode (experimental).
  static Future<Map<String, dynamic>> setDebugMode(String mode) async {
    AppLog.phase('plugin', 'setDebugMode begin', {'mode': mode});
    final result = await _plugin.setDebugMode(mode);
    AppLog.phase('plugin', 'setDebugMode complete', result);
    return result;
  }
}
