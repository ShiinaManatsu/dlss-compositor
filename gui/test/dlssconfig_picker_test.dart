import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:provider/provider.dart';
import 'package:dlss_compositor_gui/state/app_state.dart';
import 'package:dlss_compositor_gui/widgets/dlssconfig_picker.dart';

void main() {
  testWidgets('DlssConfigPicker shows open button', (WidgetTester tester) async {
    final appState = AppState();
    
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: ChangeNotifierProvider.value(
            value: appState,
            child: const DlssConfigPicker(),
          ),
        ),
      ),
    );

    expect(find.text('Open DLSS Config'), findsOneWidget);
    expect(find.byIcon(Icons.folder_open), findsOneWidget);
  });

  testWidgets('DlssConfigPicker shows error when configError is set', (WidgetTester tester) async {
    final appState = AppState();
    appState.setDlssConfig('C:\\path\\to\\invalid.json', null, 'Invalid version (expected 1, got 2)');
    
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: ChangeNotifierProvider.value(
            value: appState,
            child: const DlssConfigPicker(),
          ),
        ),
      ),
    );

    expect(find.text('C:\\path\\to\\invalid.json'), findsOneWidget);
    expect(find.text('Invalid version (expected 1, got 2)'), findsOneWidget);
  });

  testWidgets('DlssConfigPicker shows summary when config is valid', (WidgetTester tester) async {
    final appState = AppState();
    final mockConfig = {
      'version': 1,
      'output_resolution': {'width': 3840, 'height': 2160},
      'quality_mode': 'Performance',
      'is_hdr': true,
      'fps': 60,
      'frames': [{}, {}, {}], // 3 frames
    };
    appState.setDlssConfig('C:\\path\\to\\valid.json', mockConfig, null);
    
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: ChangeNotifierProvider.value(
            value: appState,
            child: const DlssConfigPicker(),
          ),
        ),
      ),
    );

    expect(find.text('C:\\path\\to\\valid.json'), findsOneWidget);
    expect(find.text('Valid'), findsOneWidget);
    expect(find.text('Out: 3840x2160'), findsOneWidget);
    expect(find.text('Performance'), findsOneWidget);
    expect(find.text('HDR'), findsOneWidget);
    expect(find.text('60 fps'), findsOneWidget);
    expect(find.text('3 frames'), findsOneWidget);
  });
}
