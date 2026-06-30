import 'package:flutter/foundation.dart';
import 'package:flutter/widgets.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'dart:async';
import 'dart:math';

import '../util/app_log.dart';

/// Persistent preferences for DLSS Compositor GUI.
/// Debug/release isolation: separate key prefix to avoid polluting release config during dev.
class AppPreferences extends ChangeNotifier {
  static const String _debugPrefix = 'dlss_compositor_debug';
  static const String _releasePrefix = 'dlss_compositor_release';

  static String get _prefix =>
      mockPrefix ?? (kDebugMode ? _debugPrefix : _releasePrefix);
  static String? mockPrefix;

  SharedPreferences? _prefs;
  Timer? _debounce;

  // Preference keys
  static const _kLastConfig = 'last_dlssconfig_path';
  static const _kPreset = 'preset_override';
  static const _kFgEnabled = 'fg_enabled';
  static const _kFgMultiplier = 'fg_multiplier';
  static const _kOutputFormat = 'output_format';
  static const _kPngQuality = 'png_quality';
  static const _kExrCompression = 'exr_compression';
  static const _kExrCompressionLevel = 'exr_compression_level';
  static const _kWriterThreads = 'writer_threads';
  static const _kWriterBufferMultiplier = 'writer_buffer_multiplier';
  static const _kH265Cq = 'h265_cq';
  static const _kH265Preset = 'h265_preset';
  static const _kH265Multipass = 'h265_multipass';
  static const _kH265SpatialAq = 'h265_spatial_aq';
  static const _kH265TemporalAq = 'h265_temporal_aq';
  static const _kH265Vfr = 'h265_vfr';
  static const _kOutputDir = 'output_directory';
  static const _kSplitterPos = 'splitter_position';
  static const _kSessionTitle = 'session_title';
  static const _kLanguageCode = 'language_code';
  static const _kBlenderExecutablePath = 'blender_executable_path';
  static const _kRegisteredFileAssociationVersion =
      'registered_file_association_version';

  // Current values with defaults
  String lastDlssConfigPath = '';
  String presetOverride = 'auto';
  bool fgEnabled = false;
  int fgMultiplier = 2;
  String outputFormat = 'exr';
  int pngQuality = 97;
  String exrCompression = 'zip';
  int exrCompressionLevel = 97;
  int writerThreads = 0;
  int writerBufferMultiplier = 3;
  int h265Cq = 18;
  String h265Preset = 'p5';
  String h265Multipass = 'qres';
  bool h265SpatialAq = true;
  bool h265TemporalAq = true;
  bool h265Vfr = false;
  String outputDirectory = '';
  double splitterPosition = 0.4;
  String sessionTitle = '';
  String languageCode = '';
  String blenderExecutablePath = '';
  int registeredFileAssociationVersion = 0;

  Locale? get localeOverride {
    switch (languageCode) {
      case 'en':
        return const Locale('en');
      case 'zh':
        return const Locale('zh', 'CN');
      default:
        return null;
    }
  }

  @override
  void dispose() {
    _debounce?.cancel();
    super.dispose();
  }

  Future<void> load() async {
    AppLog.phase('preferences', 'load begin');
    _prefs = await SharedPreferences.getInstance();
    lastDlssConfigPath = _get(_kLastConfig, '');
    presetOverride = _get(_kPreset, 'auto');
    fgEnabled = _getBool(_kFgEnabled, false);
    fgMultiplier = _getInt(_kFgMultiplier, 2);
    outputFormat = _get(_kOutputFormat, 'exr');
    pngQuality = _getInt(_kPngQuality, 97).clamp(0, 100);
    exrCompression = _get(_kExrCompression, 'zip');
    exrCompressionLevel = _getInt(_kExrCompressionLevel, 97).clamp(0, 100);
    writerThreads = _getInt(_kWriterThreads, 0).clamp(0, 32);
    writerBufferMultiplier = _getInt(_kWriterBufferMultiplier, 3).clamp(1, 64);
    h265Cq = _getInt(_kH265Cq, 18).clamp(0, 30);
    h265Preset = _get(_kH265Preset, 'p5');
    h265Multipass = _get(_kH265Multipass, 'qres');
    h265SpatialAq = _getBool(_kH265SpatialAq, true);
    h265TemporalAq = _getBool(_kH265TemporalAq, true);
    h265Vfr = _getBool(_kH265Vfr, false);
    outputDirectory = _get(_kOutputDir, '');
    splitterPosition = _getDouble(_kSplitterPos, 0.4);
    sessionTitle = _get(_kSessionTitle, '');
    languageCode = _get(_kLanguageCode, '');
    blenderExecutablePath = _get(_kBlenderExecutablePath, '');
    registeredFileAssociationVersion = _getInt(
      _kRegisteredFileAssociationVersion,
      0,
    );
    AppLog.phase('preferences', 'load complete', {
      'has_last_config': lastDlssConfigPath.isNotEmpty,
      'output_format': outputFormat,
      'has_output_directory': outputDirectory.isNotEmpty,
      'splitter_position': splitterPosition,
    });
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
    AppLog.phase('preferences', 'persist begin');
    await _prefs!.setString(_key(_kLastConfig), lastDlssConfigPath);
    await _prefs!.setString(_key(_kPreset), presetOverride);
    await _prefs!.setBool(_key(_kFgEnabled), fgEnabled);
    await _prefs!.setInt(_key(_kFgMultiplier), fgMultiplier);
    await _prefs!.setString(_key(_kOutputFormat), outputFormat);
    await _prefs!.setInt(_key(_kPngQuality), pngQuality);
    await _prefs!.setString(_key(_kExrCompression), exrCompression);
    await _prefs!.setInt(_key(_kExrCompressionLevel), exrCompressionLevel);
    await _prefs!.setInt(_key(_kWriterThreads), writerThreads);
    await _prefs!.setInt(
      _key(_kWriterBufferMultiplier),
      writerBufferMultiplier,
    );
    await _prefs!.setInt(_key(_kH265Cq), h265Cq);
    await _prefs!.setString(_key(_kH265Preset), h265Preset);
    await _prefs!.setString(_key(_kH265Multipass), h265Multipass);
    await _prefs!.setBool(_key(_kH265SpatialAq), h265SpatialAq);
    await _prefs!.setBool(_key(_kH265TemporalAq), h265TemporalAq);
    await _prefs!.setBool(_key(_kH265Vfr), h265Vfr);
    await _prefs!.setString(_key(_kOutputDir), outputDirectory);
    await _prefs!.setDouble(_key(_kSplitterPos), splitterPosition);
    await _prefs!.setString(_key(_kSessionTitle), sessionTitle);
    await _prefs!.setString(_key(_kLanguageCode), languageCode);
    await _prefs!.setString(
      _key(_kBlenderExecutablePath),
      blenderExecutablePath,
    );
    await _prefs!.setInt(
      _key(_kRegisteredFileAssociationVersion),
      registeredFileAssociationVersion,
    );
    AppLog.phase('preferences', 'persist complete');
  }

  // Setters with debounced write
  void setLastDlssConfigPath(String v) => _scheduleWrite(() {
    lastDlssConfigPath = v;
    notifyListeners();
  });
  void setPresetOverride(String v) => _scheduleWrite(() {
    presetOverride = v;
    notifyListeners();
  });
  void setFgEnabled(bool v) => _scheduleWrite(() {
    fgEnabled = v;
    notifyListeners();
  });
  void setFgMultiplier(int v) => _scheduleWrite(() {
    fgMultiplier = v;
    notifyListeners();
  });
  void setOutputFormat(String v) => _scheduleWrite(() {
    outputFormat = v;
    notifyListeners();
  });
  void setPngQuality(int v) => _scheduleWrite(() {
    pngQuality = v.clamp(0, 100);
    notifyListeners();
  });
  void setExrCompression(String v) => _scheduleWrite(() {
    exrCompression = v;
    notifyListeners();
  });
  void setExrCompressionLevel(int v) => _scheduleWrite(() {
    exrCompressionLevel = v.clamp(0, 100);
    notifyListeners();
  });
  void setWriterThreads(int v) => _scheduleWrite(() {
    writerThreads = v.clamp(0, 32);
    notifyListeners();
  });
  void setWriterBufferMultiplier(int v) => _scheduleWrite(() {
    writerBufferMultiplier = v.clamp(1, 64);
    notifyListeners();
  });
  void setH265Cq(int v) => _scheduleWrite(() {
    h265Cq = v.clamp(0, 30);
    notifyListeners();
  });
  void setH265Preset(String v) => _scheduleWrite(() {
    h265Preset = v;
    notifyListeners();
  });
  void setH265Multipass(String v) => _scheduleWrite(() {
    h265Multipass = v;
    notifyListeners();
  });
  void setH265SpatialAq(bool v) => _scheduleWrite(() {
    h265SpatialAq = v;
    notifyListeners();
  });
  void setH265TemporalAq(bool v) => _scheduleWrite(() {
    h265TemporalAq = v;
    notifyListeners();
  });
  void setH265Vfr(bool v) => _scheduleWrite(() {
    h265Vfr = v;
    notifyListeners();
  });
  void setOutputDirectory(String v) => _scheduleWrite(() {
    outputDirectory = v;
    notifyListeners();
  });
  void setSplitterPosition(double v) => _scheduleWrite(() {
    splitterPosition = v;
    notifyListeners();
  });
  void setSessionTitle(String v) => _scheduleWrite(() {
    sessionTitle = v;
    notifyListeners();
  });
  void setLanguageCode(String v) => _scheduleWrite(() {
    languageCode = v;
    notifyListeners();
  });
  void setBlenderExecutablePath(String v) => _scheduleWrite(() {
    blenderExecutablePath = v;
    notifyListeners();
  });

  void setRegisteredFileAssociationVersion(int v) => _scheduleWrite(() {
    registeredFileAssociationVersion = v;
    notifyListeners();
  });

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
    final keys = _prefs!.getKeys().where((k) => k.startsWith(_prefix)).toList();
    for (final k in keys) {
      await _prefs!.remove(k);
    }
    // Reload defaults
    lastDlssConfigPath = '';
    presetOverride = 'auto';
    fgEnabled = false;
    fgMultiplier = 2;
    outputFormat = 'exr';
    pngQuality = 97;
    exrCompression = 'zip';
    exrCompressionLevel = 97;
    writerThreads = 0;
    writerBufferMultiplier = 3;
    h265Cq = 18;
    h265Preset = 'p5';
    h265Multipass = 'qres';
    h265SpatialAq = true;
    h265TemporalAq = true;
    h265Vfr = false;
    outputDirectory = '';
    splitterPosition = 0.4;
    sessionTitle = '';
    languageCode = '';
    blenderExecutablePath = '';
    registeredFileAssociationVersion = 0;
    notifyListeners();
  }
}
