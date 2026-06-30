import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

import 'package:dlss_compositor_gui/main.dart' as app;

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('App launches and shows Open button', (
    WidgetTester tester,
  ) async {
    app.main([]);
    await tester.pumpAndSettle();
    expect(find.text('Open DLSS Config'), findsOneWidget);
  });
}
