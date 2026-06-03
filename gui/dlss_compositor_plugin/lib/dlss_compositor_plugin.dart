import 'dart:ffi';

import 'package:flutter/services.dart';

class PreviewFrame {
  final int width;
  final int height;
  final Uint8List data;

  const PreviewFrame({
    required this.width,
    required this.height,
    required this.data,
  });
}

/// Public Dart API for the DLSS Compositor Windows plugin.
class DlssCompositorPlugin {
  static const MethodChannel _methodChannel =
      MethodChannel('dlss_compositor/method');
  static const EventChannel _eventChannel =
      EventChannel('dlss_compositor/events');

  Future<bool> setTask(Map<String, dynamic> params) async {
    final result = await _methodChannel.invokeMethod<bool>('setTask', params);
    return result ?? false;
  }

  Future<bool> checkReady() async {
    final result = await _methodChannel.invokeMethod<bool>('checkReady');
    return result ?? false;
  }

  Future<Map<String, dynamic>> getStatus() async {
    final result =
        await _methodChannel.invokeMapMethod<String, dynamic>('getStatus');
    return result ?? {};
  }

  Future<Map<String, dynamic>> getProgress() async {
    final result =
        await _methodChannel.invokeMapMethod<String, dynamic>('getProgress');
    return result ?? {};
  }

  Future<Map<String, dynamic>> checkOutputCollisions(
    Map<String, dynamic> params,
  ) async {
    final result = await _methodChannel.invokeMapMethod<String, dynamic>(
      'checkOutputCollisions',
      params,
    );
    return result ?? {'collisions': 0, 'files': <String>[]};
  }

  Future<bool> startTask() async {
    final result = await _methodChannel.invokeMethod<bool>('startTask');
    return result ?? false;
  }

  Future<PreviewFrame?> getPreviewFrame() async {
    final result = await _methodChannel.invokeMapMethod<String, dynamic>(
      'getPreviewFrame',
    );
    if (result == null) return null;

    final ptr = result['ptr'] as int?;
    final size = result['size'] as int?;
    final width = result['width'] as int?;
    final height = result['height'] as int?;
    if (ptr == null || size == null || ptr == 0) return null;
    if (width == null || height == null || width <= 0 || height <= 0) return null;

    final bytes = Pointer<Uint8>.fromAddress(ptr).asTypedList(size);
    return PreviewFrame(
      width: width,
      height: height,
      data: Uint8List.fromList(bytes),
    );
  }

  Stream<Map<String, dynamic>> get progressStream {
    return _eventChannel.receiveBroadcastStream().map((event) {
      if (event is Map) {
        return Map<String, dynamic>.from(event);
      }
      return <String, dynamic>{};
    });
  }

  Future<Map<String, dynamic>> setDebugMode(String mode) async {
    final result = await _methodChannel.invokeMapMethod<String, dynamic>(
      'setDebugMode',
      {'mode': mode},
    );
    return result ?? {'ok': false, 'reason': 'not implemented'};
  }
}
