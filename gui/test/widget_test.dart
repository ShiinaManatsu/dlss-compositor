import 'package:flutter_test/flutter_test.dart';

import 'package:dlss_compositor_gui/main.dart';

void main() {
  testWidgets('App starts and shows Open button', (WidgetTester tester) async {
    await tester.pumpWidget(const DlssCompositorApp());

    expect(find.text('Open dlssconfig.json'), findsOneWidget);
    expect(find.text('DLSS Compositor v2'), findsWidgets);
  });
}
