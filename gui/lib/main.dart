import 'package:desktop_drop/desktop_drop.dart';
import 'package:fluent_ui/fluent_ui.dart' as fluent;
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:window_manager/window_manager.dart';

import 'app_version.dart';
import 'l10n/app_strings.dart';
import 'state/app_state.dart';
import 'state/preferences.dart';
import 'widgets/detail_panel.dart';
import 'widgets/status_bar.dart';
import 'widgets/task_config_panel.dart';
import 'util/app_log.dart';
import 'util/dlss_config_file.dart';
import 'util/error_dialog.dart';

Future<void> main(List<String> args) async {
  WidgetsFlutterBinding.ensureInitialized();
  if (!kIsWeb &&
      [
        TargetPlatform.windows,
        TargetPlatform.linux,
        TargetPlatform.macOS,
      ].contains(defaultTargetPlatform)) {
    await windowManager.ensureInitialized();
    const windowOptions = WindowOptions(
      title: 'DLSS Compositor $kAppVersion',
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
      child: DlssCompositorApp(initialArgs: args),
    ),
  );
  AppLog.phase('startup', 'runApp submitted');
}

class DlssCompositorApp extends StatelessWidget {
  const DlssCompositorApp({super.key, this.initialArgs = const []});

  final List<String> initialArgs;

  static const _bg = Color(0xFF090D0B);
  static const _surface = Color(0xFF111713);
  static const _surfaceAlt = Color(0xFF171F1A);
  static const _line = Color(0xFF26322B);
  static const _accent = Color(0xFF51D47A);

  @override
  Widget build(BuildContext context) {
    AppPreferences? prefs;
    try {
      prefs = context.watch<AppPreferences>();
    } on ProviderNotFoundException {
      prefs = null;
    }
    return MaterialApp(
      title: 'DLSS Compositor $kAppVersion',
      debugShowCheckedModeBanner: false,
      localizationsDelegates: fluent.FluentLocalizations.localizationsDelegates,
      supportedLocales: const [Locale('en'), Locale('zh', 'CN')],
      locale: prefs?.localeOverride,
      localeResolutionCallback: (locale, supportedLocales) {
        final languageCode = locale?.languageCode.toLowerCase();
        if (languageCode == 'zh') {
          return const Locale('zh', 'CN');
        }
        return const Locale('en');
      },
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
      home: HomePage(initialArgs: initialArgs),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key, this.initialArgs = const []});

  final List<String> initialArgs;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  double? _localSplitterPosition;
  bool _isSplitterHovered = false;
  bool _startupFileHandled = false;
  bool _isDraggingFile = false;

  double _leftWidth(double totalWidth, double splitterPosition) {
    final width = totalWidth * splitterPosition;
    final max = (totalWidth - 360).clamp(360.0, totalWidth);
    return width.clamp(360.0, max).toDouble();
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (_startupFileHandled) return;
    _startupFileHandled = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) {
        _openStartupArgument();
      }
    });
  }

  Future<void> _openStartupArgument() async {
    for (final arg in widget.initialArgs) {
      if (isDlssConfigPath(arg)) {
        await _openDlssConfigPath(arg);
        return;
      }
      if (isBlendPath(arg)) {
        Provider.of<AppState>(context, listen: false).setBlendFile(arg);
        return;
      }
    }
  }

  Future<void> _openDlssConfigPath(String path) async {
    final strings = context.strings;
    final appState = Provider.of<AppState>(context, listen: false);
    try {
      final result = await loadDlssConfigFile(
        path,
        invalidVersion: strings.invalidVersion,
        missingRequiredField: strings.missingRequiredField,
      );
      appState.setDlssConfig(path, result.config, result.error);
      if (result.error != null && mounted) {
        await showErrorDialog(
          context,
          title: strings.invalidConfigTitle,
          message: result.error!,
          details: path,
        );
      }
    } catch (e) {
      appState.setDlssConfig(path, null, strings.failedOpenConfigMessage);
      if (mounted) {
        await showErrorDialog(
          context,
          title: strings.failedOpenConfigTitle,
          message: strings.failedOpenConfigMessage,
          details: '$path\n$e',
        );
      }
    }
  }

  Future<void> _handleDroppedPath(String path) async {
    if (isDlssConfigPath(path)) {
      await _openDlssConfigPath(path);
      return;
    }
    if (isBlendPath(path)) {
      Provider.of<AppState>(context, listen: false).setBlendFile(path);
      return;
    }
    if (!mounted) return;
    await showErrorDialog(
      context,
      title: context.strings.invalidConfigTitle,
      message: context.strings.unsupportedDropMessage,
      details: path,
    );
  }

  @override
  Widget build(BuildContext context) {
    AppPreferences? prefs;
    try {
      prefs = context.watch<AppPreferences>();
    } on ProviderNotFoundException {
      prefs = null;
    }
    return DropTarget(
      onDragEntered: (_) => setState(() => _isDraggingFile = true),
      onDragExited: (_) => setState(() => _isDraggingFile = false),
      onDragDone: (details) async {
        setState(() => _isDraggingFile = false);
        if (details.files.isEmpty) return;
        await _handleDroppedPath(details.files.first.path);
      },
      child: fluent.FluentTheme(
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
            title: Text(
              '${context.strings.appTitle} $kAppVersion',
              style: const TextStyle(
                fontSize: 14,
                fontWeight: FontWeight.w700,
                color: Color(0xFFE8F2EC),
              ),
            ),
            endHeader: const Padding(
              padding: EdgeInsets.only(right: 8),
              child: _LanguageSwitcher(),
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
            body: Stack(
              children: [
                DecoratedBox(
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
                            _localSplitterPosition ??
                            prefs?.splitterPosition ??
                            0.4;
                        final leftWidth = _leftWidth(
                          totalWidth,
                          splitterPosition,
                        );

                        return Row(
                          crossAxisAlignment: CrossAxisAlignment.stretch,
                          children: [
                            SizedBox(
                              width: leftWidth,
                              child: const _WorkbenchPane(
                                child: TaskConfigPanel(),
                              ),
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
                                  final nextWidth =
                                      (leftWidth + details.delta.dx)
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
                                        : Theme.of(context).dividerColor
                                              .withValues(alpha: 0.2),
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
                if (_isDraggingFile)
                  Positioned.fill(
                    child: IgnorePointer(
                      child: DecoratedBox(
                        decoration: BoxDecoration(
                          color: DlssCompositorApp._bg.withValues(alpha: 0.78),
                          border: Border.all(
                            color: DlssCompositorApp._accent,
                            width: 2,
                          ),
                        ),
                        child: Center(
                          child: Text(
                            context.strings.dropFileToOpen,
                            style: const TextStyle(
                              color: Color(0xFFE8F2EC),
                              fontSize: 20,
                              fontWeight: FontWeight.w700,
                            ),
                          ),
                        ),
                      ),
                    ),
                  ),
              ],
            ),
            bottomNavigationBar: const DlssStatusBar(),
          ),
        ),
      ),
    );
  }
}

class _LanguageSwitcher extends StatelessWidget {
  const _LanguageSwitcher();

  @override
  Widget build(BuildContext context) {
    AppPreferences? prefs;
    try {
      prefs = context.watch<AppPreferences>();
    } on ProviderNotFoundException {
      prefs = null;
    }
    final strings = context.strings;
    final languageCode = prefs?.languageCode ?? '';
    final shortLabel = switch (languageCode) {
      'en' => strings.languageButtonEnglishShort,
      'zh' => strings.languageButtonChineseShort,
      _ => strings.languageButtonSystemShort,
    };

    return Material(
      type: MaterialType.transparency,
      child: PopupMenuButton<String>(
        tooltip: strings.language,
        initialValue: languageCode,
        onSelected: (value) => prefs?.setLanguageCode(value),
        color: const Color(0xFF111713),
        itemBuilder: (context) => [
          PopupMenuItem(
            value: '',
            child: Text('${strings.languageSystem} (${strings.languageButtonSystemShort})'),
          ),
          PopupMenuItem(
            value: 'en',
            child: Text(strings.languageEnglish),
          ),
          PopupMenuItem(
            value: 'zh',
            child: Text(strings.languageChinese),
          ),
        ],
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
          decoration: BoxDecoration(
            color: const Color(0xFF171F1A),
            border: Border.all(color: const Color(0xFF26322B)),
            borderRadius: BorderRadius.circular(10),
          ),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              const Icon(Icons.language, size: 14, color: Color(0xFFE8F2EC)),
              const SizedBox(width: 6),
              Text(
                shortLabel,
                style: const TextStyle(
                  fontSize: 11,
                  fontWeight: FontWeight.w600,
                  color: Color(0xFFE8F2EC),
                ),
              ),
            ],
          ),
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
