import 'dart:io';

import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
import 'package:flutter_device_apps_linux_example/main.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:material_ui/material_ui.dart';

const _id = 'org.example.DeviceAppsFixture.desktop';
const _hiddenId = 'org.example.HiddenFixture.desktop';
const _onlyShowInKdeId = 'org.example.OnlyShowInKdeFixture.desktop';
const _notShowInGnomeId = 'org.example.NotShowInGnomeFixture.desktop';
const _trulyHiddenId = 'org.example.TrulyHiddenFixture.desktop';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  final filePath = Platform.environment['DEVICE_APPS_TEST_FILE'];

  testWidgets('lists and searches visible desktop entries', (_) async {
    final apps = await FlutterDeviceAppsLinux.listApps();
    final fixture = apps.where((app) => app.desktopFileId == _id).toList();
    expect(fixture, hasLength(1));
    expect(fixture.single.appName, 'Device Apps Linux Fixture');
    expect(fixture.single.supportsFiles, isTrue);
    expect(fixture.single.supportsUris, isFalse);
    expect(fixture.single.requiresTerminal, isFalse);
    expect(fixture.single.description, isNull);
    expect(fixture.single.desktopFilePath, isNull);
    expect(fixture.single.categories, isEmpty);
    expect(fixture.single.mimeTypes, isEmpty);
    expect(apps.any((app) => app.desktopFileId == _hiddenId), isFalse);
    expect(apps.any((app) => app.desktopFileId == _onlyShowInKdeId), isFalse);
    expect(apps.any((app) => app.desktopFileId == _notShowInGnomeId), isFalse);
    expect(apps.any((app) => app.desktopFileId == _trulyHiddenId), isFalse);

    final concurrent = await Future.wait([
      FlutterDeviceAppsLinux.listApps(),
      FlutterDeviceAppsLinux.listApps(),
      FlutterDeviceAppsLinux.listApps(),
    ]);
    for (final batch in concurrent) {
      expect(batch.any((app) => app.desktopFileId == _id), isTrue);
      expect(batch.any((app) => app.desktopFileId == _hiddenId), isFalse);
    }

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

  testWidgets('discovers normalized applications and optional packages', (
    _,
  ) async {
    final result = await FlutterDeviceAppsLinux.discoverApps();
    expect(result.packages, isEmpty);
    expect(
      result.providerDiagnostics.any(
        (d) =>
            d.providerId == 'gio' &&
            d.status == LinuxDiscoveryProviderStatus.succeeded,
      ),
      isTrue,
    );

    final fixtureMatches = result.applications
        .where((app) => app.desktopEntries.any((e) => e.desktopFileId == _id))
        .toList();
    expect(fixtureMatches, hasLength(1));
    final fixture = fixtureMatches.single;
    expect(fixture.resolvedId, isNotEmpty);
    expect(fixture.desktopMenuVisibility, LinuxDesktopMenuVisibility.visible);
    expect(fixture.capabilities.supportsFiles, LinuxCapabilityState.supported);
    expect(
      fixture.capabilities.requiresTerminal,
      LinuxCapabilityState.unsupported,
    );

    final withPackages = await FlutterDeviceAppsLinux.discoverApps(
      includePackages: true,
      providers: const ['gio', 'dpkg'],
    );
    expect(
      withPackages.providerDiagnostics.map((d) => d.providerId),
      everyElement(isIn(['gio', 'dpkg', 'dpkg-ownership'])),
    );
    final dpkgDiag = withPackages.providerDiagnostics
        .where((d) => d.providerId == 'dpkg')
        .toList();
    if (dpkgDiag.isNotEmpty &&
        dpkgDiag.single.status == LinuxDiscoveryProviderStatus.succeeded) {
      expect(withPackages.packages, isNotEmpty);
      expect(withPackages.packages.first.packageIdentityId, isNotEmpty);
      expect(withPackages.packages.first.packageName, isNotEmpty);
    }

    // Verify concurrent execution alongside legacy listApps preserves legacy
    // GIO-only behavior.
    final concurrent = await Future.wait<Object?>([
      FlutterDeviceAppsLinux.discoverApps(includeAppStreamMetadata: false),
      FlutterDeviceAppsLinux.listApps(),
    ]);
    final legacyApps = concurrent[1]! as List<LinuxAppInfo>;
    expect(legacyApps.any((app) => app.desktopFileId == _id), isTrue);
    expect(legacyApps.any((app) => app.desktopFileId == _hiddenId), isFalse);
  });

  testWidgets('explorer example loads fixture through discoverApps', (
    tester,
  ) async {
    await tester.pumpWidget(const LinuxDeviceAppsExample());
    await tester.pumpAndSettle(const Duration(seconds: 5));

    // Switch to Applications and search for the known fixture.
    final navFinder = find.byType(NavigationRail).evaluate().isNotEmpty
        ? find.descendant(
            of: find.byType(NavigationRail),
            matching: find.text('Applications'),
          )
        : find.descendant(
            of: find.byType(NavigationBar),
            matching: find.text('Applications'),
          );
    await tester.tap(navFinder);
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const ValueKey('apps_search_field')),
      'Device Apps Linux Fixture',
    );
    await tester.pumpAndSettle();
    expect(find.text('Device Apps Linux Fixture'), findsWidgets);
  });
}
