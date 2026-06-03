import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'dart:async';
import 'dart:math';

/// Persistent preferences for DLSS Compositor GUI.
/// Debug/release isolation: separate key prefix to avoid polluting release config during dev.
class AppPreferences extends ChangeNotifier {
  static const String _debugPrefix = 'dlss_compositor_debug';
  static const String _releasePrefix = 'dlss_compositor_release';

  static String get _prefix => mockPrefix ?? (kDebugMode ? _debugPrefix : _releasePrefix);
  static String? mockPrefix;

  SharedPreferences? _prefs;
  Timer? _debounce;

  // Preference keys
  static const _kLastConfig     = 'last_dlssconfig_path';
  static const _kPreset         = 'preset_override';
  static const _kFgEnabled      = 'fg_enabled';
  static const _kFgMultiplier   = 'fg_multiplier';
  static const _kOutputFormat   = 'output_format';
  static const _kOutputDir      = 'output_directory';
  static const _kSplitterPos    = 'splitter_position';
  static const _kSessionTitle   = 'session_title';

  // Current values with defaults
  String lastDlssConfigPath = '';
  String presetOverride = 'auto';
  bool fgEnabled = false;
  int fgMultiplier = 2;
  String outputFormat = 'exr';
  String outputDirectory = '';
  double splitterPosition = 0.4;
  String sessionTitle = '';

  @override
  void dispose() {
    _debounce?.cancel();
    super.dispose();
  }

  Future<void> load() async {
    _prefs = await SharedPreferences.getInstance();
    lastDlssConfigPath  = _get(_kLastConfig, '');
    presetOverride      = _get(_kPreset, 'auto');
    fgEnabled           = _getBool(_kFgEnabled, false);
    fgMultiplier        = _getInt(_kFgMultiplier, 2);
    outputFormat        = _get(_kOutputFormat, 'exr');
    outputDirectory     = _get(_kOutputDir, '');
    splitterPosition    = _getDouble(_kSplitterPos, 0.4);
    sessionTitle        = _get(_kSessionTitle, '');
    notifyListeners();
  }

  // Prefixed getters
  String _key(String k) => '${_prefix}_$k';
  String _get(String k, String def) => _prefs?.getString(_key(k)) ?? def;
  bool _getBool(String k, bool def) => _prefs?.getBool(_key(k)) ?? def;
  int _getInt(String k, int def) => _prefs?.getInt(_key(k)) ?? def;
  double _getDouble(String k, double def) => _prefs?.getDouble(_key(k)) ?? def;

  // Debounced save (500ms delay)
  void _scheduleWrite(VoidCallback fn) {
    fn();
    _debounce?.cancel();
    _debounce = Timer(const Duration(milliseconds: 500), _persist);
  }

  Future<void> _persist() async {
    if (_prefs == null) return;
    await _prefs!.setString(_key(_kLastConfig), lastDlssConfigPath);
    await _prefs!.setString(_key(_kPreset), presetOverride);
    await _prefs!.setBool(_key(_kFgEnabled), fgEnabled);
    await _prefs!.setInt(_key(_kFgMultiplier), fgMultiplier);
    await _prefs!.setString(_key(_kOutputFormat), outputFormat);
    await _prefs!.setString(_key(_kOutputDir), outputDirectory);
    await _prefs!.setDouble(_key(_kSplitterPos), splitterPosition);
    await _prefs!.setString(_key(_kSessionTitle), sessionTitle);
  }

  // Setters with debounced write
  void setLastDlssConfigPath(String v) => _scheduleWrite(() { lastDlssConfigPath = v; notifyListeners(); });
  void setPresetOverride(String v) => _scheduleWrite(() { presetOverride = v; notifyListeners(); });
  void setFgEnabled(bool v) => _scheduleWrite(() { fgEnabled = v; notifyListeners(); });
  void setFgMultiplier(int v) => _scheduleWrite(() { fgMultiplier = v; notifyListeners(); });
  void setOutputFormat(String v) => _scheduleWrite(() { outputFormat = v; notifyListeners(); });
  void setOutputDirectory(String v) => _scheduleWrite(() { outputDirectory = v; notifyListeners(); });
  void setSplitterPosition(double v) => _scheduleWrite(() { splitterPosition = v; notifyListeners(); });
  void setSessionTitle(String v) => _scheduleWrite(() { sessionTitle = v; notifyListeners(); });

  /// Generate a session title from the dlssconfig path if none is set.
  /// Format: `<aov_dir_name>_<random4>`
  void deriveSessionTitleIfEmpty(String configPath) {
    if (sessionTitle.isNotEmpty || configPath.isEmpty) return;
    
    final normalized = configPath.replaceAll('\\', '/');
    final segments = normalized.split('/');
    if (segments.length > 1) {
      final aovDir = segments[segments.length - 2];
      final random = Random().nextInt(10000).toString().padLeft(4, '0');
      setSessionTitle('${aovDir}_$random');
    }
  }

  /// Reset all preferences to defaults.
  Future<void> resetToDefaults() async {
    if (_prefs == null) return;
    // Remove all keys with our prefix
    final keys = _prefs!.getKeys()
        .where((k) => k.startsWith(_prefix))
        .toList();
    for (final k in keys) {
      await _prefs!.remove(k);
    }
    // Reload defaults
    lastDlssConfigPath  = '';
    presetOverride      = 'auto';
    fgEnabled           = false;
    fgMultiplier        = 2;
    outputFormat        = 'exr';
    outputDirectory     = '';
    splitterPosition    = 0.4;
    sessionTitle        = '';
    notifyListeners();
  }
}
