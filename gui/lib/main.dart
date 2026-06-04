import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'state/app_state.dart';
import 'state/preferences.dart';
import 'widgets/detail_panel.dart';
import 'widgets/status_bar.dart';
import 'widgets/task_config_panel.dart';
import 'util/app_log.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  AppLog.resetStartup();
  AppLog.phase('startup', 'Flutter binding initialized');
  runApp(
    MultiProvider(
      providers: [
        ChangeNotifierProvider(create: (_) => AppState()..loadStatus()),
        ChangeNotifierProvider(create: (_) => AppPreferences()..load()),
      ],
      child: const DlssCompositorApp(),
    ),
  );
  AppLog.phase('startup', 'runApp submitted');
}

class DlssCompositorApp extends StatelessWidget {
  const DlssCompositorApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'DLSS Compositor v2',
      theme: ThemeData(
        colorSchemeSeed: Colors.green,
        useMaterial3: true,
      ),
      home: const HomePage(),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  double? _localSplitterPosition;

  double _leftWidth(double totalWidth, double splitterPosition) {
    final width = totalWidth * splitterPosition;
    final max = (totalWidth - 360).clamp(360.0, totalWidth);
    return width.clamp(360.0, max).toDouble();
  }

  @override
  Widget build(BuildContext context) {
    AppPreferences? prefs;
    try {
      prefs = context.watch<AppPreferences>();
    } on ProviderNotFoundException {
      prefs = null;
    }
    return Scaffold(
      appBar: AppBar(title: const Text('DLSS Compositor v2')),
      body: Padding(
        padding: const EdgeInsets.all(16.0),
        child: LayoutBuilder(
          builder: (context, constraints) {
            final totalWidth = constraints.maxWidth;
            final splitterPosition = _localSplitterPosition ?? prefs?.splitterPosition ?? 0.4;
            final leftWidth = _leftWidth(totalWidth, splitterPosition);

            return Row(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                SizedBox(
                  width: leftWidth,
                  child: const TaskConfigPanel(),
                ),
                const SizedBox(width: 6),
                MouseRegion(
                  cursor: SystemMouseCursors.resizeColumn,
                  child: GestureDetector(
                    behavior: HitTestBehavior.opaque,
                    onPanUpdate: (details) {
                      final nextWidth = (leftWidth + details.delta.dx)
                          .clamp(360.0, totalWidth - 360.0)
                          .toDouble();
                      setState(() => _localSplitterPosition = nextWidth / totalWidth);
                    },
                    onPanEnd: (_) {
                      final next = _localSplitterPosition;
                      if (next != null && prefs != null) {
                        prefs.setSplitterPosition(next);
                      }
                    },
                    child: Container(
                      width: 8,
                      decoration: BoxDecoration(
                        color: Theme.of(context).dividerColor.withValues(alpha: 0.35),
                        borderRadius: BorderRadius.circular(8),
                      ),
                    ),
                  ),
                ),
                const SizedBox(width: 6),
                const Expanded(child: DetailPanel()),
              ],
            );
          },
        ),
      ),
      bottomNavigationBar: const DlssStatusBar(),
    );
  }
}

