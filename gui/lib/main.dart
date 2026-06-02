import 'package:flutter/material.dart';

import 'util/win_file_dialog.dart';

void main() {
  runApp(const DlssCompositorApp());
}

class DlssCompositorApp extends StatelessWidget {
  const DlssCompositorApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'DLSS Compositor v2',
      theme: ThemeData(colorSchemeSeed: Colors.green, useMaterial3: true),
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
  String? _selectedPath;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('DLSS Compositor v2')),
      body: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            ElevatedButton(
              onPressed: _openFile,
              child: const Text('Open dlssconfig.json'),
            ),
            if (_selectedPath != null)
              Padding(
                padding: const EdgeInsets.all(8.0),
                child: Text('Selected: $_selectedPath'),
              ),
          ],
        ),
      ),
    );
  }

  void _openFile() {
    final path = selectFile(
      filterDescription: 'DLSS Config',
      filterPattern: '*.json',
    );
    if (path != null) {
      setState(() => _selectedPath = path);
    }
  }
}
