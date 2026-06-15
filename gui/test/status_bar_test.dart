import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:provider/provider.dart';
import 'package:dlss_compositor_gui/app_version.dart';
import 'package:dlss_compositor_gui/state/app_state.dart';
import 'package:dlss_compositor_gui/widgets/status_bar.dart';

void main() {
  testWidgets('DlssStatusBar shows connecting when empty', (WidgetTester tester) async {
    final appState = AppState();
    
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          bottomNavigationBar: ChangeNotifierProvider.value(
            value: appState,
            child: const DlssStatusBar(),
          ),
        ),
      ),
    );

    expect(find.text('DLSS Compositor $kAppVersion - Connecting...'), findsOneWidget);
  });

  testWidgets('DlssStatusBar shows DLSS info when populated', (WidgetTester tester) async {
    final appState = AppState();
    appState.dlssVersion = '3.5.10';
    appState.gpuName = 'RTX 4090';
    appState.fgMaxMultiplier = 4;
    
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          bottomNavigationBar: ChangeNotifierProvider.value(
            value: appState,
            child: const DlssStatusBar(),
          ),
        ),
      ),
    );

    expect(find.text('DLSS Compositor $kAppVersion | DLSS 3.5.10 | RTX 4090 | FG Max 4x'), findsOneWidget);
  });

  testWidgets('DlssStatusBar shows progress when running', (WidgetTester tester) async {
    final appState = AppState();
    appState.dlssVersion = '3.5.10';
    appState.gpuName = 'RTX 4090';
    appState.setTaskState(TaskState.running);
    appState.updateProgress(10, 100, 0.1);
    
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          bottomNavigationBar: ChangeNotifierProvider.value(
            value: appState,
            child: const DlssStatusBar(),
          ),
        ),
      ),
    );

    expect(find.text('10/100'), findsOneWidget);
    expect(find.byType(LinearProgressIndicator), findsNothing);
  });
}
