import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'state/app_state.dart';
import 'widgets/dlssconfig_picker.dart';
import 'widgets/status_bar.dart';

void main() {
  runApp(
    ChangeNotifierProvider(
      create: (_) => AppState()..loadStatus(),
      child: const DlssCompositorApp(),
    ),
  );
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

class HomePage extends StatelessWidget {
  const HomePage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('DLSS Compositor v2')),
      body: const Padding(
        padding: EdgeInsets.all(16.0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            DlssConfigPicker(),
          ],
        ),
      ),
      bottomNavigationBar: const DlssStatusBar(),
    );
  }
}

