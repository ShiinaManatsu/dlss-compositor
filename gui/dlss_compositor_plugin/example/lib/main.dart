import 'package:flutter/material.dart';

import 'package:dlss_compositor_plugin/dlss_compositor_plugin.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  final plugin = DlssCompositorPlugin();
  late Future<Map<String, dynamic>> status;

  @override
  void initState() {
    super.initState();
    status = plugin.getStatus();
  }

  @override
  Widget build(BuildContext context) {
    const textStyle = TextStyle(fontSize: 25);
    const spacerSmall = SizedBox(height: 10);
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(title: const Text('Native Packages')),
        body: SingleChildScrollView(
          child: Container(
            padding: const .all(10),
            child: Column(
              children: [
                const Text(
                  'This calls a native function through FFI that is shipped as source in the package. '
                  'The native code is built as part of the Flutter Runner build.',
                  style: textStyle,
                  textAlign: .center,
                ),
                spacerSmall,
                FutureBuilder<Map<String, dynamic>>(
                  future: status,
                  builder: (BuildContext context, AsyncSnapshot<Map<String, dynamic>> value) {
                    final displayValue = value.hasData
                        ? value.data.toString()
                        : 'Loading status...';
                    return Text(
                      displayValue,
                      style: textStyle,
                      textAlign: .center,
                    );
                  },
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
