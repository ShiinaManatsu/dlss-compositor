import 'package:dlss_compositor_gui/plugin/dlss_compositor.dart';
import 'dart:io';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const methodChannel = MethodChannel('dlss_compositor/method');
  final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

  tearDown(() async {
    messenger.setMockMethodCallHandler(methodChannel, null);
    messenger.setMockMessageHandler('dlss_compositor/events', null);
  });

  test('setTask forwards params to native method channel', () async {
    MethodCall? receivedCall;
    messenger.setMockMethodCallHandler(methodChannel, (call) async {
      receivedCall = call;
      return true;
    });

    final ok = await DlssCompositor.setTask({'config': 'fixture.json'});

    expect(ok, isTrue);
    expect(receivedCall?.method, 'setTask');
    expect(receivedCall?.arguments, {'config': 'fixture.json'});
  });

  test('progressStream receives native event channel progress event', () async {
    const codec = StandardMethodCodec();
    messenger.setMockMessageHandler('dlss_compositor/events', (message) async {
      final call = codec.decodeMethodCall(message);
      if (call.method == 'listen') {
        await messenger.handlePlatformMessage(
          'dlss_compositor/events',
          codec.encodeSuccessEnvelope({
            'type': 'progress',
            'current_frame': 1,
            'total_frames': 10,
          }),
          (_) {},
        );
        return codec.encodeSuccessEnvelope(null);
      }
      if (call.method == 'cancel') {
        return codec.encodeSuccessEnvelope(null);
      }
      fail('Unexpected event channel call: ${call.method}');
    });

    final event = await DlssCompositor.progressStream.first;

    expect(event['type'], 'progress');
    expect(event['current_frame'], 1);
    expect(event['total_frames'], 10);
  });

  test('checkOutputCollisions scans output directory from task params', () async {
    final dir = Directory.systemTemp.createTempSync('dlss_gui_collision_test');
    addTearDown(() => dir.deleteSync(recursive: true));
    File('${dir.path}${Platform.pathSeparator}frame_0001.exr').writeAsStringSync('exists');

    final result = await DlssCompositor.checkOutputCollisions({
      'output_dir': dir.path,
      'template': 'frame_####.exr',
      'frame_count': 2,
    });

    expect(result['collisions'], 1);
    expect(result['files'], isA<List<String>>());
  });
}
