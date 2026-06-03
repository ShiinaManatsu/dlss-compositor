import 'package:dlss_compositor_plugin/dlss_compositor_plugin.dart';

/// Dart wrapper for DLSS Compositor native plugin.
///
/// Communicates via MethodChannel (commands), EventChannel (events), and FFI
/// pointers for zero-copy preview frame reads.
class DlssCompositor {
  static final DlssCompositorPlugin _plugin = DlssCompositorPlugin();

  /// Send a task configuration to the native engine.
  static Future<bool> setTask(Map<String, dynamic> params) async {
    return _plugin.setTask(params);
  }

  /// Check if the engine is ready to process tasks.
  static Future<bool> checkReady() async {
    return _plugin.checkReady();
  }

  /// Get DLSS status (version, GPU, SR/FG availability).
  static Future<Map<String, dynamic>> getStatus() async {
    return _plugin.getStatus();
  }

  /// Get current render progress.
  static Future<Map<String, dynamic>> getProgress() async {
    return _plugin.getProgress();
  }

  /// Start the render task.
  static Future<bool> startTask() async {
    return _plugin.startTask();
  }

  /// Preflight output path collisions for the current task settings.
  static Future<Map<String, dynamic>> checkOutputCollisions(
    Map<String, dynamic> params,
  ) async {
    return _plugin.checkOutputCollisions(params);
  }

  /// Get latest preview frame via FFI shared memory (zero-copy).
  static Future<PreviewFrame?> getPreviewFrame() async {
    return _plugin.getPreviewFrame();
  }

  /// Stream of progress and event updates from the native engine.
  static Stream<Map<String, dynamic>> get progressStream {
    return _plugin.progressStream;
  }

  /// Set debug mode (experimental).
  static Future<Map<String, dynamic>> setDebugMode(String mode) async {
    return _plugin.setDebugMode(mode);
  }
}
