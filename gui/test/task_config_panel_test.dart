import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:provider/provider.dart';
import 'package:dlss_compositor_gui/state/app_state.dart';
import 'package:dlss_compositor_gui/widgets/task_config_panel.dart';
import 'package:flutter/services.dart';

void main() {
  setUpAll(() {
    TestWidgetsFlutterBinding.ensureInitialized();
    // Stub the method channel for the plugin
    const channel = MethodChannel('dlss_compositor_plugin');
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(channel, (MethodCall methodCall) async {
      if (methodCall.method == 'getStatus') {
        return {
          'dlss_version': '3.5.0',
          'gpu_name': 'RTX 4090',
          'fg_max_multiplier': 4,
          'sr_available': true,
          'fg_available': true,
        };
      }
      return null;
    });
  });

  Widget buildTestApp(AppState appState) {
    return MaterialApp(
      home: Scaffold(
        body: ChangeNotifierProvider<AppState>.value(
          value: appState,
          child: const TaskConfigPanel(),
        ),
      ),
    );
  }

  testWidgets('TaskConfigPanel displays formats correctly for LDR', (WidgetTester tester) async {
    final appState = AppState();
    // Set LDR config
    appState.setDlssConfig('C:\\config.json', {
      'is_hdr': false,
      'output_resolution': {'width': 1920, 'height': 1080},
      'quality_mode': 'Performance',
      'fps': 60,
    }, null);

    await tester.pumpWidget(buildTestApp(appState));
    await tester.pumpAndSettle();

    // Check available formats in the dropdown
    // LDR should have PNG and EXR.
    expect(find.text('PNG (16-bit)'), findsOneWidget);
    
    // Check elements
    expect(find.text('Output: 1920×1080'), findsOneWidget);
    expect(find.text('Quality: Performance'), findsOneWidget);
    expect(find.text('FPS: 60'), findsOneWidget);
  });

  testWidgets('TaskConfigPanel limits formats for HDR', (WidgetTester tester) async {
    final appState = AppState();
    // Set HDR config
    appState.setDlssConfig('C:\\config.json', {
      'is_hdr': true,
      'output_resolution': {'width': 3840, 'height': 2160},
      'quality_mode': 'Quality',
      'fps': 120,
    }, null);

    await tester.pumpWidget(buildTestApp(appState));
    await tester.pumpAndSettle();

    // EXR Linear should be the default format selected since PNG is not available
    expect(find.text('EXR (scene-linear)'), findsOneWidget);
    
    // Let's open the dropdown
    await tester.tap(find.text('EXR (scene-linear)'));
    await tester.pumpAndSettle();
    
    // Dropdown items should only include EXR.
    expect(find.text('EXR (scene-linear)'), findsWidgets);
    expect(find.text('ProRes 422 HQ'), findsNothing);
    // PNG should not be available
    expect(find.text('PNG (16-bit)'), findsNothing);
  });

  testWidgets('TaskConfigPanel handles FG multiplier limits', (WidgetTester tester) async {
    final appState = AppState();
    appState.fgAvailable = true;
    appState.fgMaxMultiplier = 3;
    appState.setDlssConfig('C:\\config.json', {}, null);

    await tester.pumpWidget(buildTestApp(appState));
    await tester.pumpAndSettle();

    // Enable FG
    final switchFinder = find.byType(Switch);
    expect(switchFinder, findsOneWidget);
    await tester.tap(switchFinder);
    await tester.pumpAndSettle();

    // Check choice chips
    expect(find.text('2x'), findsOneWidget);
    expect(find.text('3x'), findsOneWidget);
    expect(find.text('4x'), findsOneWidget);

    // 2x and 3x should be enabled, 4x should be disabled
    ChoiceChip chip4x = tester.widget(find.widgetWithText(ChoiceChip, '4x'));
    expect(chip4x.onSelected, isNull);

    ChoiceChip chip3x = tester.widget(find.widgetWithText(ChoiceChip, '3x'));
    expect(chip3x.onSelected, isNotNull);
  });

  testWidgets('Template formats dynamically based on extension', (WidgetTester tester) async {
    final appState = AppState();
    appState.setDlssConfig('C:\\config.json', {'is_hdr': false}, null);

    await tester.pumpWidget(buildTestApp(appState));
    await tester.pumpAndSettle();

    // Initial state: frame_#### -> frame_0001.png (because PNG is first for LDR)
    expect(find.text('→ frame_0001.png'), findsOneWidget);

    // Change template
    await tester.enterText(find.byType(TextField), 'img_####');
    await tester.pumpAndSettle();
    expect(find.text('→ img_0001.png'), findsOneWidget);

    // Add explicit extension
    await tester.enterText(find.byType(TextField), 'img_####.exr');
    await tester.pumpAndSettle();
    expect(find.text('→ img_0001.png'), findsOneWidget);
  });

  testWidgets('Template extension follows output format', (WidgetTester tester) async {
    final appState = AppState();
    appState.setDlssConfig('C:\\config.json', {'is_hdr': false}, null);

    await tester.pumpWidget(buildTestApp(appState));
    await tester.pumpAndSettle();

    await tester.tap(find.text('PNG (16-bit)'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('EXR (scene-linear)').last);
    await tester.pumpAndSettle();

    expect(find.text('→ frame_0001.exr'), findsOneWidget);
  });

}
