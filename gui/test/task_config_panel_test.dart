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
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall methodCall) async {
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

  testWidgets('TaskConfigPanel displays formats correctly for LDR', (
    WidgetTester tester,
  ) async {
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
    expect(find.text('Out: 1920x1080'), findsOneWidget);
    expect(find.text('Performance'), findsOneWidget);
    expect(find.text('60 fps'), findsOneWidget);
  });

  testWidgets('TaskConfigPanel accepts Blender LUT config shape', (
    WidgetTester tester,
  ) async {
    final appState = AppState();
    appState.setDlssConfig('F:\\New Folder\\dlssconfig.json', {
      'version': 1,
      'output_resolution': {'width': 1215, 'height': 2160},
      'render_resolution': {'width': 814, 'height': 1447},
      'quality_mode': 'quality',
      'display_device': 'Rec.2020',
      'view_transform': 'ACES 2.0',
      'is_hdr': false,
      'fps': 30,
      'tonemap_lut': 'tonemap_Rec.2020_ACES_2.0_33.cube',
      'inverse_lut': 'inverse_Rec.2020_ACES_2.0_33.cube',
      'frames': {
        '0000': {'jitter_x': 0.0, 'jitter_y': -0.16666666666666669},
        '0001': {'jitter_x': -0.25, 'jitter_y': 0.16666666666666663},
      },
    }, null);

    await tester.pumpWidget(buildTestApp(appState));
    await tester.pumpAndSettle();

    expect(find.text('In: 814x1447'), findsOneWidget);
    expect(find.text('Out: 1215x2160'), findsOneWidget);
    expect(find.text('quality'), findsOneWidget);
    expect(find.text('30 fps'), findsOneWidget);
    expect(find.text('2 frames'), findsWidgets);
    expect(find.text('PNG (16-bit)'), findsOneWidget);
  });

  testWidgets('TaskConfigPanel limits formats for HDR', (
    WidgetTester tester,
  ) async {
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

    // Dropdown items should include EXR and HDR-capable video outputs, but not PNG.
    expect(find.text('EXR (scene-linear)'), findsWidgets);
    expect(find.text('ProRes 422 HQ (.mov)'), findsWidgets);
    expect(find.text('H.265 HEVC (.mp4)'), findsWidgets);
    // PNG should not be available
    expect(find.text('PNG (16-bit)'), findsNothing);
  });

  testWidgets('TaskConfigPanel handles FG multiplier limits', (
    WidgetTester tester,
  ) async {
    final appState = AppState();
    appState.fgAvailable = true;
    appState.fgMaxMultiplier = 6;
    appState.setDlssConfig('C:\\config.json', {}, null);

    await tester.pumpWidget(buildTestApp(appState));
    await tester.pumpAndSettle();

    // Enable FG
    final switchFinder = find.byType(Switch);
    expect(switchFinder, findsOneWidget);
    await tester.tap(switchFinder);
    await tester.pumpAndSettle();

    for (final multiplier in [2, 3, 4, 5, 6]) {
      expect(find.text('${multiplier}x'), findsOneWidget);
      final chip = tester.widget<ChoiceChip>(
        find.widgetWithText(ChoiceChip, '${multiplier}x'),
      );
      expect(chip.onSelected, isNotNull);
    }
    expect(
      find.descendant(of: find.byType(Expanded), matching: find.byType(Wrap)),
      findsWidgets,
    );
    expect(find.text('7x'), findsNothing);
  });

  testWidgets('TaskConfigPanel shows format-specific encoder options', (
    WidgetTester tester,
  ) async {
    final appState = AppState();
    appState.setDlssConfig('C:\\config.json', {'is_hdr': false}, null);

    await tester.pumpWidget(buildTestApp(appState));
    await tester.pumpAndSettle();

    expect(find.text('PNG Quality'), findsOneWidget);
    expect(find.text('97%'), findsOneWidget);

    await tester.tap(find.text('PNG (16-bit)'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('EXR (scene-linear)').last);
    await tester.pumpAndSettle();

    expect(find.text('EXR Compression'), findsOneWidget);
    expect(find.text('ZIP'), findsOneWidget);
    expect(find.text('Compression'), findsOneWidget);
  });

  testWidgets('Template formats dynamically based on extension', (
    WidgetTester tester,
  ) async {
    final appState = AppState();
    appState.setDlssConfig('C:\\config.json', {'is_hdr': false}, null);

    await tester.pumpWidget(buildTestApp(appState));
    await tester.pumpAndSettle();

    // Initial state: extensionless frame_#### -> frame_0001.png (because PNG is first for LDR)
    expect(find.text('-> frame_0001.png'), findsOneWidget);

    // Change template
    await tester.enterText(
      find.byKey(const Key('filenameTemplateField')),
      'img_####',
    );
    await tester.pumpAndSettle();
    expect(find.text('-> img_0001.png'), findsOneWidget);

    // Explicit extension in the template is normalized to the selected format.
    await tester.enterText(
      find.byKey(const Key('filenameTemplateField')),
      'img_####.exr',
    );
    await tester.pumpAndSettle();
    expect(find.text('-> img_0001.png'), findsOneWidget);
  });

  testWidgets('Template extension follows output format', (
    WidgetTester tester,
  ) async {
    final appState = AppState();
    appState.setDlssConfig('C:\\config.json', {'is_hdr': false}, null);

    await tester.pumpWidget(buildTestApp(appState));
    await tester.pumpAndSettle();

    await tester.tap(find.text('PNG (16-bit)'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('EXR (scene-linear)').last);
    await tester.pumpAndSettle();

    expect(find.text('-> frame_0001.exr'), findsOneWidget);
  });

  testWidgets(
    'Template text remains extensionless when output format changes',
    (WidgetTester tester) async {
      final appState = AppState();
      appState.setDlssConfig('C:\\config.json', {'is_hdr': false}, null);

      await tester.pumpWidget(buildTestApp(appState));
      await tester.pumpAndSettle();

      await tester.enterText(
        find.byKey(const Key('filenameTemplateField')),
        'shot_####.png',
      );
      await tester.pumpAndSettle();
      expect(appState.template, 'shot_####');
      expect(find.text('-> shot_0001.png'), findsOneWidget);

      await tester.tap(find.text('PNG (16-bit)'));
      await tester.pumpAndSettle();
      await tester.tap(find.text('EXR (scene-linear)').last);
      await tester.pumpAndSettle();

      final field = tester.widget<TextField>(
        find.byKey(const Key('filenameTemplateField')),
      );
      expect(field.controller?.text, 'shot_####');
      expect(appState.template, 'shot_####');
      expect(find.text('-> shot_0001.exr'), findsOneWidget);
    },
  );

  testWidgets('Frame range limits expected output frame count', (
    WidgetTester tester,
  ) async {
    final appState = AppState();
    appState.setDlssConfig('C:\\config.json', {
      'is_hdr': false,
      'frames': {'0000': {}, '0001': {}, '0002': {}, '0003': {}},
    }, null);

    await tester.pumpWidget(buildTestApp(appState));
    await tester.pumpAndSettle();

    expect(find.text('4 frames'), findsWidgets);

    await tester.enterText(find.byKey(const Key('frameRangeStartField')), '1');
    await tester.enterText(find.byKey(const Key('frameRangeEndField')), '2');
    await tester.pumpAndSettle();

    expect(appState.frameStart, 1);
    expect(appState.frameEnd, 2);
    expect(find.text('2 frames'), findsWidgets);
  });
}
