import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('flutter_device_apps_linux/methods');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const id = 'org.example.Editor.desktop';
  const minimal = {'desktopFileId': id, 'appName': 'Editor'};

  setUp(() => debugDefaultTargetPlatformOverride = TargetPlatform.linux);
  tearDown(() {
    messenger.setMockMethodCallHandler(channel, null);
    debugDefaultTargetPlatformOverride = null;
  });

  test('resolves a desktop ID and decodes optional metadata', () async {
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'getApp');
      expect(call.arguments, id);
      return {
        ...minimal,
        'description': 'A text editor',
        'desktopFilePath': '/usr/share/applications/$id',
        'categories': ['Utility', 'TextEditor'],
        'mimeTypes': ['text/plain'],
      };
    });
    final app = (await FlutterDeviceAppsLinux.getApp(id))!;
    expect(app.appName, 'Editor');
    expect(app.desktopFileId, id);
    expect(app.description, 'A text editor');
    expect(app.desktopFilePath, '/usr/share/applications/$id');
    expect(app.categories, ['Utility', 'TextEditor']);
    expect(app.mimeTypes, ['text/plain']);
  });

  test('distinguishes missing app from missing optional details', () async {
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => call.arguments == id ? minimal : null,
    );
    final app = (await FlutterDeviceAppsLinux.getApp(id))!;
    expect(app.description, isNull);
    expect(app.desktopFilePath, isNull);
    expect(app.categories, isEmpty);
    expect(app.mimeTypes, isEmpty);
    expect(await FlutterDeviceAppsLinux.getApp('Missing.desktop'), isNull);
  });

  test('keeps decoding the original listApps response', () async {
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'listApps');
      expect(call.arguments, isNull);
      return [minimal];
    });
    final app = (await FlutterDeviceAppsLinux.listApps()).single;
    expect(app.desktopFileId, id);
    expect(app.appName, 'Editor');
    expect(app.categories, isEmpty);
    expect(app.mimeTypes, isEmpty);
  });

  test('rejects paths and invalid IDs before invoking the channel', () async {
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => fail('Unexpected channel call'),
    );
    for (final input in [
      '',
      '/tmp/$id',
      '../$id',
      'editor',
      'a\u0000.desktop',
    ]) {
      await expectLater(
        FlutterDeviceAppsLinux.getApp(input),
        throwsArgumentError,
      );
    }
    debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
    await expectLater(
      FlutterDeviceAppsLinux.getApp(id),
      throwsUnsupportedError,
    );
  });
}
