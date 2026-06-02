import 'package:dlss_compositor_gui/plugin/dlss_compositor.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('native plugin accepts setTask and emits progress event', (tester) async {
    expect(await DlssCompositor.checkReady(), isTrue);
    expect(await DlssCompositor.setTask({'config': 'integration.json'}), isTrue);

    final firstEvent = DlssCompositor.progressStream.first;
    expect(await DlssCompositor.startTask(), isTrue);

    final event = await firstEvent.timeout(const Duration(seconds: 5));
    expect(event['type'], 'progress');
    expect(event['current_frame'], isA<int>());
    expect(event['total_frames'], 10);
  });
}
