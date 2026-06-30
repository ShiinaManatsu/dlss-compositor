import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:dlss_compositor_gui/util/dlss_config_file.dart';

void main() {
  String invalidVersion(Object? got) => 'Invalid version: $got';
  String missingRequiredField(String field) => 'Missing field: $field';

  Future<DlssConfigLoadResult> load(String path) {
    return loadDlssConfigFile(
      path,
      invalidVersion: invalidVersion,
      missingRequiredField: missingRequiredField,
    );
  }

  test('recognizes dlssc, legacy json, and blend paths', () {
    expect(isDlssConfigPath('scene.dlssc'), isTrue);
    expect(isDlssConfigPath('scene.DLSSC'), isTrue);
    expect(isDlssConfigPath('dlssconfig.json'), isTrue);
    expect(isDlssConfigPath('scene.blend'), isFalse);
    expect(isBlendPath('scene.blend'), isTrue);
  });

  test('loads .dlssc through shared JSON config loader', () async {
    final dir = await Directory.systemTemp.createTemp('dlssc_loader_');
    addTearDown(() => dir.delete(recursive: true));
    final file = File('${dir.path}${Platform.pathSeparator}config.dlssc');
    await file.writeAsString('''
{
  "version": 1,
  "output_resolution": {"width": 1920, "height": 1080},
  "fps": 60,
  "frames": {}
}
''');

    final result = await load(file.path);

    expect(result.isValid, isTrue);
    expect(result.config?['version'], 1);
    expect(result.error, isNull);
  });

  test('loads legacy .json configs through the same path', () async {
    final dir = await Directory.systemTemp.createTemp('dlssc_loader_');
    addTearDown(() => dir.delete(recursive: true));
    final file = File('${dir.path}${Platform.pathSeparator}dlssconfig.json');
    await file.writeAsString('''
{
  "version": 1,
  "output_resolution": {"width": 1280, "height": 720},
  "fps": 30
}
''');

    final result = await load(file.path);

    expect(result.isValid, isTrue);
    expect(result.config?['fps'], 30);
  });

  test('validation remains schema based, not extension based', () async {
    final dir = await Directory.systemTemp.createTemp('dlssc_loader_');
    addTearDown(() => dir.delete(recursive: true));
    final file = File('${dir.path}${Platform.pathSeparator}bad.dlssc');
    await file.writeAsString('{"version": 2}');

    final result = await load(file.path);

    expect(result.isValid, isFalse);
    expect(result.error, 'Invalid version: 2');
  });
}
