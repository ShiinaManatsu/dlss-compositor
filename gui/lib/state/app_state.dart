import 'package:flutter/foundation.dart';
import '../plugin/dlss_compositor.dart';

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
  String? taskError;

  // Task configuration fields
  String preset = 'auto';
  bool fgEnabled = false;
  int fgMultiplier = 2;
  String outputFormat = 'png_16';
  String overwriteMode = 'overwrite';
  String outputDir = '';
  String template = 'frame_####.png';

  // Load DLSS status from native plugin
  Future<void> loadStatus() async {
    try {
      final ready = await DlssCompositor.checkReady();
      engineReady = ready;
      final status = await DlssCompositor.getStatus();
      dlssVersion = status['dlss_version'] as String? ?? '';
      gpuName = status['gpu_name'] as String? ?? '';
      fgMaxMultiplier = status['fg_max_multiplier'] as int? ?? 2;
      srAvailable = status['sr_available'] as bool? ?? false;
      fgAvailable = status['fg_available'] as bool? ?? false;
      connectionError = false;
      notifyListeners();
    } catch (e) {
      debugPrint('Failed to load DLSS status: $e');
    }
  }

  // Set selected dlssconfig
  void setDlssConfig(String path, Map<String, dynamic>? config, String? error) {
    currentDlssConfigPath = path;
    currentDlssConfig = config;
    configError = error;
    notifyListeners();
  }

  // Update progress
  void updateProgress(int frame, int total, double percent) {
    currentFrame = frame;
    totalFrames = total;
    progressPercent = percent;
    notifyListeners();
  }

  void setTaskState(TaskState state, {String? error}) {
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
    String? outputDir,
    String? template,
  }) {
    if (preset != null) this.preset = preset;
    if (fgEnabled != null) this.fgEnabled = fgEnabled;
    if (fgMultiplier != null) this.fgMultiplier = fgMultiplier;
    if (outputFormat != null) this.outputFormat = outputFormat;
    if (overwriteMode != null) this.overwriteMode = overwriteMode;
    if (outputDir != null) this.outputDir = outputDir;
    if (template != null) this.template = template;
    notifyListeners();
  }
}
