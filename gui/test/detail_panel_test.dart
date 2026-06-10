import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:provider/provider.dart';
import 'package:dlss_compositor_gui/state/app_state.dart';
import 'package:dlss_compositor_gui/widgets/detail_panel.dart';

void main() {
  testWidgets('DetailPanel renders sections correctly based on state', (WidgetTester tester) async {
    final appState = AppState();

    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: ChangeNotifierProvider<AppState>.value(
            value: appState,
            child: const DetailPanel(),
          ),
        ),
      ),
    );

    // Initial state: TaskState.idle
    expect(find.text('TERMINAL LOG'), findsOneWidget);
    expect(find.text('Live Preview'), findsOneWidget);

    // Update to running state
    appState.taskState = TaskState.running;
    appState.updateProgress(10, 100, 0.1);

    await tester.pump();
    expect(find.text('PROCESSING'), findsOneWidget);
    expect(find.text('Disk Output'), findsOneWidget);
    
    // Update to done state
    appState.setTaskConfig(outputDir: 'C:\\output');
    appState.setTaskState(TaskState.done);
    await tester.pump();

    expect(find.text('COMPLETED'), findsOneWidget);
    expect(find.text('PROCESS COMPLETED'), findsOneWidget);
    expect(find.text('Open Folder'), findsOneWidget);
    final openButton = tester.widget<FilledButton>(find.widgetWithText(FilledButton, 'Open Folder'));
    expect(openButton.onPressed, isNotNull);
  });
}
