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
  const path = '/tmp/document.txt';
  const app = {
    'desktopFileId': id,
    'appName': 'Editor',
    'mimeTypes': ['text/plain'],
  };

  setUp(() => debugDefaultTargetPlatformOverride = TargetPlatform.linux);
  tearDown(() {
    messenger.setMockMethodCallHandler(channel, null);
    debugDefaultTargetPlatformOverride = null;
  });

  test('decodes icon bytes and null, with the original desktop ID', () async {
    final png = Uint8List.fromList([137, 80, 78, 71]);
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'getAppIcon');
      return call.arguments == id ? png : null;
    });
    expect(await FlutterDeviceAppsLinux.getAppIcon(id), png);
    expect(await FlutterDeviceAppsLinux.getAppIcon('Missing.desktop'), isNull);
  });

  test('opens apps and preserves launch errors', () async {
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'openApp');
      if (call.arguments == 'Error.desktop') {
        throw PlatformException(code: 'ERR_OPEN');
      }
      return call.arguments == id;
    });
    expect(await FlutterDeviceAppsLinux.openApp(id), isTrue);
    expect(await FlutterDeviceAppsLinux.openApp('Missing.desktop'), isFalse);
    await expectLater(
      FlutterDeviceAppsLinux.openApp('Error.desktop'),
      throwsA(
        isA<PlatformException>().having((e) => e.code, 'code', 'ERR_OPEN'),
      ),
    );
  });

  test('queries default and available handlers for absolute paths', () async {
    messenger.setMockMethodCallHandler(channel, (call) async {
      if (call.arguments != path) return null;
      return switch (call.method) {
        'getDefaultAppForFile' => app,
        'listAppsForFile' => [app],
        _ => fail('Unexpected method'),
      };
    });
    expect(
      (await FlutterDeviceAppsLinux.getDefaultAppForFile(path))?.desktopFileId,
      id,
    );
    expect(
      (await FlutterDeviceAppsLinux.listAppsForFile(path)).single.mimeTypes,
      ['text/plain'],
    );
    expect(
      await FlutterDeviceAppsLinux.getDefaultAppForFile('/tmp/missing'),
      isNull,
    );
    expect(
      await FlutterDeviceAppsLinux.listAppsForFile('/tmp/missing'),
      isEmpty,
    );
  });

  test('decodes packaging type, identifiers, and unknown source', () async {
    Map<String, String>? response = {'source': 'snap', 'packageId': 'example'};
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'getInstallSourceInfo');
      expect(call.arguments, id);
      return response;
    });
    final snap = (await FlutterDeviceAppsLinux.getInstallSourceInfo(id))!;
    expect(snap.source, LinuxAppInstallSource.snap);
    expect(snap.packageId, 'example');
    response = {'source': 'appImage', 'versionName': '1.2.3'};
    final image = (await FlutterDeviceAppsLinux.getInstallSourceInfo(id))!;
    expect(image.source, LinuxAppInstallSource.appImage);
    expect(image.versionName, '1.2.3');
    response = {'source': 'future-format'};
    expect(
      (await FlutterDeviceAppsLinux.getInstallSourceInfo(id))?.source,
      LinuxAppInstallSource.unknown,
    );
    response = null;
    expect(await FlutterDeviceAppsLinux.getInstallSourceInfo(id), isNull);
  });

  test('categorizes and deduplicates exact ELF library names', () async {
    List<String>? dependencies = [
      'libQt6Core.so.6',
      'libQt5Core.so.5',
      '/local/lib/libQt6Quick.so.6.3',
      'libgtk-4.so.1',
      'libSDL3.so.0',
      'libSDL2-2.0.so.0',
      'libflutter_linux_gtk.so',
      'libvlc.so.5',
      'libQt6Core-spoof.so.6',
      'libvulkan.so.1.bak',
    ];
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'detectAppTechnologies');
      expect(call.arguments, id);
      return dependencies;
    });
    final found = (await FlutterDeviceAppsLinux.detectAppTechnologies(id))!;
    expect(found.frameworks, [
      LinuxAppTechnology.flutter,
      LinuxAppTechnology.qt,
    ]);
    expect(found.uiToolkits, [
      LinuxAppTechnology.flutter,
      LinuxAppTechnology.qtQuick,
      LinuxAppTechnology.gtk4,
    ]);
    expect(found.libraries, [
      LinuxAppTechnology.qt,
      LinuxAppTechnology.sdl2,
      LinuxAppTechnology.sdl3,
      LinuxAppTechnology.libVlc,
    ]);
    dependencies = [];
    expect(
      (await FlutterDeviceAppsLinux.detectAppTechnologies(id))?.libraries,
      isEmpty,
    );
    dependencies = null;
    expect(await FlutterDeviceAppsLinux.detectAppTechnologies(id), isNull);
  });

  test('validates lookup arguments before invoking native code', () async {
    messenger.setMockMethodCallHandler(
      channel,
      (_) async => fail('Unexpected channel call'),
    );
    await expectLater(
      FlutterDeviceAppsLinux.getAppIcon('/tmp/$id'),
      throwsArgumentError,
    );
    await expectLater(
      FlutterDeviceAppsLinux.openApp('bad'),
      throwsArgumentError,
    );
    await expectLater(
      FlutterDeviceAppsLinux.getDefaultAppForFile('relative.txt'),
      throwsArgumentError,
    );
    await expectLater(
      FlutterDeviceAppsLinux.listAppsForFile('/tmp/\u0000'),
      throwsArgumentError,
    );
    debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
    await expectLater(
      FlutterDeviceAppsLinux.detectAppTechnologies(id),
      throwsUnsupportedError,
    );
  });
}
