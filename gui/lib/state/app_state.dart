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

  // Load DLSS status from native plugin
  Future<void> loadStatus() async {
    try {
      final status = await DlssCompositor.getStatus();
      dlssVersion = status['dlss_version'] as String? ?? '';
      gpuName = status['gpu_name'] as String? ?? '';
      fgMaxMultiplier = status['fg_max_multiplier'] as int? ?? 2;
      srAvailable = status['sr_available'] as bool? ?? false;
      fgAvailable = status['fg_available'] as bool? ?? false;
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

  void setTaskState(TaskState state) {
    taskState = state;
    notifyListeners();
  }
}
