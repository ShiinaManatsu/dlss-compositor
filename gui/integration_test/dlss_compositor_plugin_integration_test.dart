import 'package:dlss_compositor_gui/plugin/dlss_compositor.dart';
import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('Flutter entry reports CLI backend input errors', (tester) async {
    final root = _projectRoot();
    final outputDir = Directory('${root.path}${Platform.pathSeparator}.omo${Platform.pathSeparator}evidence${Platform.pathSeparator}flutter-entry-e2e');
    if (outputDir.existsSync()) outputDir.deleteSync(recursive: true);

    expect(await DlssCompositor.checkReady(), isTrue);
    expect(await DlssCompositor.setTask({
      'config': '${root.path}${Platform.pathSeparator}tests${Platform.pathSeparator}fixtures${Platform.pathSeparator}sample-dlssconfig.json',
      'output_dir': outputDir.path,
      'template': 'frame_####.png',
      'output_format': 'png_16',
      'overwrite_mode': 'overwrite',
      'frame_count': 5,
    }), isTrue);

    final error = DlssCompositor.progressStream.firstWhere(
      (event) => event['type'] == 'error',
    );
    expect(await DlssCompositor.startTask(), isTrue);
    final event = await error.timeout(const Duration(seconds: 10));
    expect(event['message'], contains('CLI exited with code 4'));
  });
}

Directory _projectRoot() {
  var dir = Directory.current.absolute;
  while (true) {
    if (File('${dir.path}${Platform.pathSeparator}AGENTS.md').existsSync() &&
        File('${dir.path}${Platform.pathSeparator}CMakeLists.txt').existsSync()) {
      return dir;
    }
    final parent = dir.parent;
    if (parent.path == dir.path) return Directory.current.absolute;
    dir = parent;
  }
}
