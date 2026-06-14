import 'package:fluent_ui/fluent_ui.dart' as fluent;
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:window_manager/window_manager.dart';

import 'state/app_state.dart';
import 'state/preferences.dart';
import 'widgets/detail_panel.dart';
import 'widgets/status_bar.dart';
import 'widgets/task_config_panel.dart';
import 'util/app_log.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  if (!kIsWeb &&
      [
        TargetPlatform.windows,
        TargetPlatform.linux,
        TargetPlatform.macOS,
      ].contains(defaultTargetPlatform)) {
    await windowManager.ensureInitialized();
    const windowOptions = WindowOptions(
      title: 'DLSS Compositor v2',
      titleBarStyle: TitleBarStyle.hidden,
      windowButtonVisibility: false,
      minimumSize: Size(960, 640),
      backgroundColor: Colors.transparent,
    );
    windowManager.waitUntilReadyToShow(windowOptions, () async {
      await windowManager.show();
      await windowManager.focus();
    });
  }
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

  static const _bg = Color(0xFF090D0B);
  static const _surface = Color(0xFF111713);
  static const _surfaceAlt = Color(0xFF171F1A);
  static const _line = Color(0xFF26322B);
  static const _accent = Color(0xFF51D47A);

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'DLSS Compositor v2',
      debugShowCheckedModeBanner: false,
      localizationsDelegates: fluent.FluentLocalizations.localizationsDelegates,
      supportedLocales: fluent.FluentLocalizations.supportedLocales,
      themeMode: ThemeMode.dark,
      darkTheme: ThemeData(
        brightness: Brightness.dark,
        colorSchemeSeed: _accent,
        useMaterial3: true,
        scaffoldBackgroundColor: _bg,
        fontFamily: 'Segoe UI',
        dividerColor: _line,
        cardTheme: const CardThemeData(
          color: _surface,
          elevation: 0,
          shape: RoundedRectangleBorder(
            side: BorderSide(color: _line),
            borderRadius: BorderRadius.all(Radius.circular(16)),
          ),
        ),
        chipTheme: ChipThemeData(
          backgroundColor: _surfaceAlt,
          selectedColor: _accent.withValues(alpha: 0.22),
          side: const BorderSide(color: _line),
          labelStyle: const TextStyle(fontSize: 12),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(999),
          ),
        ),
        inputDecorationTheme: InputDecorationTheme(
          filled: true,
          fillColor: _bg.withValues(alpha: 0.65),
          border: OutlineInputBorder(borderRadius: BorderRadius.circular(12)),
          enabledBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(12),
            borderSide: const BorderSide(color: _line),
          ),
          focusedBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(12),
            borderSide: const BorderSide(color: _accent),
          ),
        ),
        filledButtonTheme: FilledButtonThemeData(
          style: FilledButton.styleFrom(
            backgroundColor: _accent,
            foregroundColor: const Color(0xFF041008),
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(14),
            ),
          ),
        ),
        elevatedButtonTheme: ElevatedButtonThemeData(
          style: ElevatedButton.styleFrom(
            backgroundColor: _accent,
            foregroundColor: const Color(0xFF041008),
            elevation: 0,
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(14),
            ),
          ),
        ),
        appBarTheme: const AppBarTheme(
          backgroundColor: _bg,
          scrolledUnderElevation: 0,
          elevation: 0,
          toolbarHeight: 56,
          titleTextStyle: TextStyle(
            fontSize: 15,
            fontWeight: FontWeight.w700,
            color: Color(0xFFE8F2EC),
            letterSpacing: 0.2,
          ),
        ),
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
  bool _isSplitterHovered = false;

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
    return fluent.FluentTheme(
      data: fluent.FluentThemeData(
        brightness: Brightness.dark,
        accentColor: fluent.Colors.green,
        fontFamily: 'Segoe UI',
      ),
      child: fluent.NavigationView(
        titleBar: fluent.TitleBar(
          height: 44,
          isBackButtonVisible: false,
          icon: fluent.Padding(
            padding: const EdgeInsets.only(left: 16),
            child: const Icon(
              Icons.auto_awesome_motion,
              size: 18,
              color: DlssCompositorApp._accent,
            ),
          ),
          title: const Text(
            'DLSS Compositor v2',
            style: TextStyle(
              fontSize: 14,
              fontWeight: FontWeight.w700,
              color: Color(0xFFE8F2EC),
            ),
          ),
          captionControls: const _WindowButtons(),
          onDragStarted: !kIsWeb ? windowManager.startDragging : null,
          onDoubleTap: !kIsWeb
              ? () async {
                  if (await windowManager.isMaximized()) {
                    await windowManager.unmaximize();
                  } else {
                    await windowManager.maximize();
                  }
                }
              : null,
        ),
        content: Scaffold(
          body: DecoratedBox(
            decoration: const BoxDecoration(
              gradient: LinearGradient(
                begin: Alignment.topLeft,
                end: Alignment.bottomRight,
                colors: [
                  Color(0xFF090D0B),
                  Color(0xFF0E1511),
                  Color(0xFF070A09),
                ],
              ),
            ),
            child: Padding(
              padding: const EdgeInsets.fromLTRB(16, 8, 16, 14),
              child: LayoutBuilder(
                builder: (context, constraints) {
                  final totalWidth = constraints.maxWidth;
                  final splitterPosition =
                      _localSplitterPosition ?? prefs?.splitterPosition ?? 0.4;
                  final leftWidth = _leftWidth(totalWidth, splitterPosition);

                  return Row(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      SizedBox(
                        width: leftWidth,
                        child: const _WorkbenchPane(child: TaskConfigPanel()),
                      ),
                      MouseRegion(
                        cursor: SystemMouseCursors.resizeColumn,
                        onEnter: (_) =>
                            setState(() => _isSplitterHovered = true),
                        onExit: (_) =>
                            setState(() => _isSplitterHovered = false),
                        child: GestureDetector(
                          behavior: HitTestBehavior.opaque,
                          onPanUpdate: (details) {
                            final nextWidth = (leftWidth + details.delta.dx)
                                .clamp(360.0, totalWidth - 360.0)
                                .toDouble();
                            setState(
                              () => _localSplitterPosition =
                                  nextWidth / totalWidth,
                            );
                          },
                          onPanEnd: (_) {
                            final next = _localSplitterPosition;
                            if (next != null && prefs != null) {
                              prefs.setSplitterPosition(next);
                            }
                            setState(() => _isSplitterHovered = false);
                          },
                          child: Container(
                            width: 16,
                            alignment: Alignment.center,
                            child: Container(
                              width: _isSplitterHovered ? 2 : 1,
                              color: _isSplitterHovered
                                  ? Theme.of(context).colorScheme.primary
                                  : Theme.of(
                                      context,
                                    ).dividerColor.withValues(alpha: 0.2),
                            ),
                          ),
                        ),
                      ),
                      const Expanded(
                        child: _WorkbenchPane(child: DetailPanel()),
                      ),
                    ],
                  );
                },
              ),
            ),
          ),
          bottomNavigationBar: const DlssStatusBar(),
        ),
      ),
    );
  }
}

class _WindowButtons extends StatelessWidget {
  const _WindowButtons();

  @override
  Widget build(BuildContext context) {
    final theme = fluent.FluentTheme.of(context);
    return SizedBox(
      width: 138,
      height: 44,
      child: WindowCaption(
        brightness: theme.brightness,
        backgroundColor: Colors.transparent,
      ),
    );
  }
}

class _WorkbenchPane extends StatelessWidget {
  const _WorkbenchPane({required this.child});

  final Widget child;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: const Color(0xFF0E1411).withValues(alpha: 0.94),
        border: Border.all(color: const Color(0xFF26322B)),
        borderRadius: BorderRadius.circular(18),
        boxShadow: const [
          BoxShadow(
            color: Color(0x66000000),
            blurRadius: 30,
            offset: Offset(0, 16),
          ),
        ],
      ),
      child: ClipRRect(borderRadius: BorderRadius.circular(17), child: child),
    );
  }
}
