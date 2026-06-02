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
    expect(find.text('Ready'), findsOneWidget);
    expect(find.text('Log'), findsOneWidget);
    expect(find.text('Preview frames will appear here during processing'), findsOneWidget);

    // Update to running state
    appState.taskState = TaskState.running;
    appState.updateProgress(10, 100, 0.1);

    await tester.pump();
    expect(find.text('Processing: 10/100'), findsOneWidget);
    expect(find.text('10.0%'), findsOneWidget);
    
    // Update to done state
    appState.setTaskState(TaskState.done);
    await tester.pump();

    expect(find.text('Complete ✓'), findsOneWidget);
    expect(find.text('Completed'), findsOneWidget);
    expect(find.text('Open Output Folder'), findsOneWidget);
  });
}
