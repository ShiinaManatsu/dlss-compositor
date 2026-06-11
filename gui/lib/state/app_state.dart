import 'package:flutter/foundation.dart';
import '../plugin/dlss_compositor.dart';
import '../util/app_log.dart';

/// Task state enum
enum TaskState { idle, running, done, error }

/// Global application state managed by Provider.
class AppState extends ChangeNotifier {
  // DLSS config
  String? currentDlssConfigPath;
  Map<String, dynamic>? currentDlssConfig;
  String? configError;

  // Status from getStatus RPC
  bool engineReady = false;
  bool connectionError = false;
  String dlssVersion = '';
  String gpuName = '';
  int fgMaxMultiplier = 2;
  bool srAvailable = false;
  bool fgAvailable = false;

  // Task state
  TaskState taskState = TaskState.idle;
  int currentFrame = 0;
  int totalFrames = 0;
  double progressPercent = 0.0;
  int dlssCurrentFrame = 0;
  int dlssTotalFrames = 0;
  double dlssProgressPercent = 0.0;
  String? taskError;

  // Task configuration fields
  String preset = 'auto';
  bool fgEnabled = false;
  int fgMultiplier = 2;
  String outputFormat = 'png_16';
  String overwriteMode = 'overwrite';
  int pngQuality = 97;
  String exrCompression = 'zip';
  int exrCompressionLevel = 97;
  String outputDir = '';
  String template = 'frame_####';

  // Load DLSS status from native plugin
  Future<void> loadStatus() async {
    AppLog.phase('status', 'loadStatus begin');
    try {
      final ready = await DlssCompositor.checkReady();
      engineReady = ready;
      AppLog.phase('status', 'checkReady complete', {'ready': ready});
      final status = await DlssCompositor.getStatus();
      dlssVersion = status['dlss_version'] as String? ?? '';
      gpuName = status['gpu_name'] as String? ?? '';
      fgMaxMultiplier = status['fg_max_multiplier'] as int? ?? 2;
      srAvailable = status['sr_available'] as bool? ?? false;
      fgAvailable = status['fg_available'] as bool? ?? false;
      if (!fgAvailable || fgMaxMultiplier < 2) {
        fgEnabled = false;
      }
      if (fgMaxMultiplier >= 2 && fgMultiplier > fgMaxMultiplier) {
        fgMultiplier = fgMaxMultiplier;
      }
      connectionError = false;
      AppLog.phase('status', 'getStatus complete', status);
      notifyListeners();
    } catch (e) {
      connectionError = true;
      AppLog.phase('status', 'loadStatus failed', e);
      debugPrint('Failed to load DLSS status: $e');
    }
  }

  // Set selected dlssconfig
  void setDlssConfig(String path, Map<String, dynamic>? config, String? error) {
    AppLog.phase('config', 'setDlssConfig', {
      ...AppLog.summarizePath(path),
      'valid': config != null,
      'error': error,
    });
    currentDlssConfigPath = path;
    currentDlssConfig = config;
    configError = error;
    notifyListeners();
  }

  // Update progress
  void updateProgress(
    int frame,
    int total,
    double percent, {
    int? dlssFrame,
    int? dlssTotal,
    double? dlssPercent,
  }) {
    AppLog.phase('progress', 'updateProgress', {
      'frame': frame,
      'total': total,
      'percent': percent,
      'dlss_frame': dlssFrame,
      'dlss_total': dlssTotal,
      'dlss_percent': dlssPercent,
    });
    currentFrame = frame;
    totalFrames = total;
    progressPercent = percent;
    dlssCurrentFrame = dlssFrame ?? frame;
    dlssTotalFrames = dlssTotal ?? total;
    dlssProgressPercent = dlssPercent ?? percent;
    notifyListeners();
  }

  void setTaskState(TaskState state, {String? error}) {
    AppLog.phase('task', 'setTaskState', {'state': state.name, 'error': error});
    taskState = state;
    taskError = error;
    notifyListeners();
  }

  void setTaskConfig({
    String? preset,
    bool? fgEnabled,
    int? fgMultiplier,
    String? outputFormat,
    String? overwriteMode,
    int? pngQuality,
    String? exrCompression,
    int? exrCompressionLevel,
    String? outputDir,
    String? template,
  }) {
    if (preset != null) this.preset = preset;
    if (fgEnabled != null) this.fgEnabled = fgEnabled;
    if (fgMultiplier != null) this.fgMultiplier = fgMultiplier;
    if (outputFormat != null) this.outputFormat = outputFormat;
    if (overwriteMode != null) this.overwriteMode = overwriteMode;
    if (pngQuality != null) this.pngQuality = pngQuality.clamp(0, 100);
    if (exrCompression != null) this.exrCompression = exrCompression;
    if (exrCompressionLevel != null) this.exrCompressionLevel = exrCompressionLevel.clamp(0, 100);
    if (outputDir != null) this.outputDir = outputDir;
    if (template != null) this.template = template;
    notifyListeners();
  }
}
