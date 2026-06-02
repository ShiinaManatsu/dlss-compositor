import 'dart:ffi';

import 'package:flutter/services.dart';

/// Dart wrapper for DLSS Compositor native plugin.
///
/// Communicates via MethodChannel (commands), EventChannel (events), and FFI
/// pointers for zero-copy preview frame reads.
class DlssCompositor {
  static const MethodChannel _methodChannel =
      MethodChannel('dlss_compositor/method');
  static const EventChannel _eventChannel =
      EventChannel('dlss_compositor/events');

  /// Send a task configuration to the native engine.
  static Future<bool> setTask(Map<String, dynamic> params) async {
    final result = await _methodChannel.invokeMethod<bool>('setTask', params);
    return result ?? false;
  }

  /// Check if the engine is ready to process tasks.
  static Future<bool> checkReady() async {
    final result = await _methodChannel.invokeMethod<bool>('checkReady');
    return result ?? false;
  }

  /// Get DLSS status (version, GPU, SR/FG availability).
  static Future<Map<String, dynamic>> getStatus() async {
    final result =
        await _methodChannel.invokeMapMethod<String, dynamic>('getStatus');
    return result ?? {};
  }

  /// Get current render progress.
  static Future<Map<String, dynamic>> getProgress() async {
    final result =
        await _methodChannel.invokeMapMethod<String, dynamic>('getProgress');
    return result ?? {};
  }

  /// Start the render task.
  static Future<bool> startTask() async {
    final result = await _methodChannel.invokeMethod<bool>('startTask');
    return result ?? false;
  }

  /// Get latest preview frame via FFI shared memory (zero-copy).
  static Future<Uint8List?> getPreviewFrame() async {
    final result = await _methodChannel.invokeMapMethod<String, dynamic>(
      'getPreviewFrame',
    );
    if (result == null) return null;

    final ptr = result['ptr'] as int?;
    final size = result['size'] as int?;
    if (ptr == null || size == null || ptr == 0) return null;

    final pointer = Pointer<Uint8>.fromAddress(ptr);
    return pointer.asTypedList(size);
  }

  /// Stream of progress and event updates from the native engine.
  static Stream<Map<String, dynamic>> get progressStream {
    return _eventChannel.receiveBroadcastStream().map((event) {
      if (event is Map) {
        return Map<String, dynamic>.from(event);
      }
      return <String, dynamic>{};
    });
  }

  /// Set debug mode (experimental).
  static Future<Map<String, dynamic>> setDebugMode(String mode) async {
    final result = await _methodChannel.invokeMapMethod<String, dynamic>(
      'setDebugMode',
      {'mode': mode},
    );
    return result ?? {'ok': false, 'reason': 'not implemented'};
  }
}
