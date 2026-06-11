import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:dlss_compositor_gui/state/preferences.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  setUp(() {
    SharedPreferences.setMockInitialValues({});
  });

  test('AppPreferences - loads defaults when empty', () async {
    final prefs = AppPreferences();
    await prefs.load();
    expect(prefs.presetOverride, 'auto');
    expect(prefs.fgEnabled, false);
    expect(prefs.fgMultiplier, 2);
    expect(prefs.pngQuality, 97);
    expect(prefs.exrCompression, 'zip');
    expect(prefs.exrCompressionLevel, 97);
  });

  test('AppPreferences - set and persist values', () async {
    final prefs = AppPreferences();
    await prefs.load();
    prefs.setFgMultiplier(4);
    prefs.setFgEnabled(true);
    prefs.setPngQuality(83);
    prefs.setExrCompression('dwaa');
    prefs.setExrCompressionLevel(91);
    await Future.delayed(const Duration(milliseconds: 600));
    expect(prefs.fgMultiplier, 4);
    expect(prefs.fgEnabled, true);
    expect(prefs.pngQuality, 83);
    expect(prefs.exrCompression, 'dwaa');
    expect(prefs.exrCompressionLevel, 91);
  });

  test('AppPreferences - reset to defaults', () async {
    final prefs = AppPreferences();
    await prefs.load();
    prefs.setFgMultiplier(4);
    await prefs.resetToDefaults();
    expect(prefs.fgMultiplier, 2);
    expect(prefs.presetOverride, 'auto');
    expect(prefs.pngQuality, 97);
    expect(prefs.exrCompression, 'zip');
    expect(prefs.exrCompressionLevel, 97);
  });

  test('AppPreferences - has persistable settings', () async {
    final prefs = AppPreferences();
    await prefs.load();
    // Verify all 8 settings have default values
    expect(prefs.lastDlssConfigPath, isA<String>());
    expect(prefs.presetOverride, isA<String>());
    expect(prefs.fgEnabled, isA<bool>());
    expect(prefs.fgMultiplier, isA<int>());
    expect(prefs.outputFormat, isA<String>());
    expect(prefs.pngQuality, isA<int>());
    expect(prefs.exrCompression, isA<String>());
    expect(prefs.exrCompressionLevel, isA<int>());
    expect(prefs.outputDirectory, isA<String>());
    expect(prefs.splitterPosition, isA<double>());
    expect(prefs.sessionTitle, isA<String>());
  });
}
