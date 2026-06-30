import 'package:flutter_test/flutter_test.dart';
import 'package:provider/provider.dart';

import 'package:dlss_compositor_gui/app_version.dart';
import 'package:dlss_compositor_gui/main.dart';
import 'package:dlss_compositor_gui/state/app_state.dart';

void main() {
  testWidgets('App starts and shows Open button', (WidgetTester tester) async {
    await tester.pumpWidget(
      ChangeNotifierProvider(
        create: (_) => AppState(),
        child: const DlssCompositorApp(),
      ),
    );

    expect(find.text('Open DLSS Config'), findsOneWidget);
    expect(find.text('DLSS Compositor $kAppVersion'), findsWidgets);
  });
}
