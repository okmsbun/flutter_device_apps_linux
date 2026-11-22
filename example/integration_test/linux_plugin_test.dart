import 'dart:io';

import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

const _id = 'org.example.DeviceAppsFixture.desktop';
const _hiddenId = 'org.example.HiddenFixture.desktop';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  final filePath = Platform.environment['DEVICE_APPS_TEST_FILE'];

  testWidgets('lists and searches visible desktop entries', (_) async {
    final apps = await FlutterDeviceAppsLinux.listApps();
    expect(apps.any((app) => app.desktopFileId == _id), isTrue);
    expect(apps.any((app) => app.desktopFileId == _hiddenId), isFalse);

    final search = await FlutterDeviceAppsLinux.searchApps('deviceappsfixture');
    expect(search.map((app) => app.desktopFileId), [_id]);
    expect(search.single.supportsFiles, isTrue);
    expect(search.single.requiresTerminal, isFalse);

    final hidden = await FlutterDeviceAppsLinux.getApp(_hiddenId);
    expect(hidden?.supportsUris, isTrue);
    expect(hidden?.requiresTerminal, isTrue);
  });

  testWidgets('reads app details and requests a launch', (_) async {
    final app = await FlutterDeviceAppsLinux.getApp(_id);
    expect(app?.description, 'Fixture for Linux plugin testing');
    expect(app?.mimeTypes, contains('text/plain'));
    expect(app?.categories, contains('Utility'));
    final icon = await FlutterDeviceAppsLinux.getAppIcon(_id);
    expect(icon?.take(8), orderedEquals([137, 80, 78, 71, 13, 10, 26, 10]));
    expect(await FlutterDeviceAppsLinux.openApp(_id), isTrue);
    expect(await FlutterDeviceAppsLinux.openApp('Missing.desktop'), isFalse);
  });

  testWidgets('resolves handlers by file path and MIME type', (_) async {
    final path = filePath!;
    final defaultForFile = await FlutterDeviceAppsLinux.getDefaultAppForFile(
      path,
    );
    final defaultForType =
        await FlutterDeviceAppsLinux.getDefaultAppForMimeType('text/plain');
    expect(defaultForFile?.desktopFileId, _id);
    expect(defaultForType?.desktopFileId, _id);

    final fileHandlers = await FlutterDeviceAppsLinux.listAppsForFile(path);
    final typeHandlers = await FlutterDeviceAppsLinux.listAppsForMimeType(
      'text/plain',
    );
    expect(fileHandlers.map((app) => app.desktopFileId), contains(_id));
    expect(typeHandlers.map((app) => app.desktopFileId), contains(_id));
  });

  testWidgets('looks up local AppStream and packaging metadata', (_) async {
    final metadata = await FlutterDeviceAppsLinux.getAppMetadata(_id);
    expect(metadata?.developer, 'Example Team');
    expect(metadata?.license, 'MIT');
    expect(metadata?.description, 'Opens fixture text files.');
    final source = await FlutterDeviceAppsLinux.getInstallSourceInfo(_id);
    expect(source?.source, LinuxAppInstallSource.flatpak);
    expect(source?.packageId, 'org.example.DeviceAppsFixture');

    final technologies = await FlutterDeviceAppsLinux.detectAppTechnologies(
      _id,
    );
    expect(technologies?.libraries, isEmpty);
  });

  testWidgets('detects a library linked to an ELF desktop executable', (
    _,
  ) async {
    final technologies = await FlutterDeviceAppsLinux.detectAppTechnologies(
      'org.example.ELFFixture.desktop',
    );
    expect(technologies?.libraries, contains(LinuxAppTechnology.sdl3));
  });
}
