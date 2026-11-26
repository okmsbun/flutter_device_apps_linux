import 'package:flutter/services.dart';
import 'package:flutter_device_apps_linux_example/main.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:material_ui/material_ui.dart';

void main() {
  const channel = MethodChannel('flutter_device_apps_linux/methods');
  const editorDesktopId = 'org.example.Editor.desktop';
  const browserDesktopId = 'org.example.Browser.desktop';
  const editorLegacy = {'desktopFileId': editorDesktopId, 'appName': 'Editor'};
  const browserLegacy = {
    'desktopFileId': browserDesktopId,
    'appName': 'Browser',
  };
  const linux = TargetPlatformVariant({TargetPlatform.linux});

  Map<String, Object?> sampleDiscoveryPayload({bool includePackages = true}) =>
      {
        'applications': [
          {
            'resolvedId': 'flatpak:user:org.example.Editor:user/stable/x86_64',
            'primaryProvenance': {
              'providerId': 'flatpak',
              'nativeId': 'org.example.Editor',
              'installationSource': 'flatpak',
              'installationScope': 'user',
            },
            'sourceProvenances': [
              {
                'providerId': 'flatpak',
                'nativeId': 'org.example.Editor',
                'installationSource': 'flatpak',
                'installationScope': 'user',
              },
              {
                'providerId': 'gio',
                'nativeId': editorDesktopId,
                'installationSource': 'unknown',
                'installationScope': 'unknown',
              },
            ],
            'originalRecordIds': [
              'flatpak:user:org.example.Editor:user/stable/x86_64',
              'gio:unknown:$editorDesktopId',
            ],
            'desktopEntries': [
              {
                'desktopFileId': editorDesktopId,
                'desktopFilePath': '/usr/share/applications/$editorDesktopId',
                'displayName': 'Example Editor',
                'desktopMenuVisibility': 'visible',
                'capabilities': {
                  'supportsFiles': 'supported',
                  'supportsUris': 'unsupported',
                  'requiresTerminal': 'unsupported',
                  'launchable': 'supported',
                },
                'providerId': 'gio',
                'originalApplicationId': 'gio:unknown:$editorDesktopId',
              },
            ],
            'linkedPackageIdentityIds': <String>[],
            'linkedAppStreamComponentIds': ['org.example.Editor'],
            'metadata': {
              'name': 'Example Editor',
              'version': '2.1.0',
              'summary': 'Modern local text editor',
              'license': 'MIT',
              'architecture': 'x86_64',
              'branch': 'stable',
            },
            'catalogReleases': {
              'latestVersion': '2.3.0',
              'latestDate': '2026-03-01',
              'releases': [
                {'version': '2.3.0', 'date': '2026-03-01'},
              ],
            },
            'capabilities': {
              'supportsFiles': 'supported',
              'supportsUris': 'unsupported',
              'requiresTerminal': 'unsupported',
              'launchable': 'supported',
            },
            'desktopMenuVisibility': 'visible',
            'classification': 'guiApplication',
            'metadataConflicts': [
              {
                'fieldName': 'summary',
                'chosenProviderId': 'appstream',
                'chosenValue': 'Modern local text editor',
                'conflictingProviderId': 'gio',
                'conflictingValue': 'Plain editor',
              },
            ],
            'ambiguities': <Object?>[],
          },
          {
            'resolvedId': 'snap:system:cli-helper',
            'primaryProvenance': {
              'providerId': 'snap',
              'nativeId': 'cli-helper',
              'installationSource': 'snap',
              'installationScope': 'system',
            },
            'sourceProvenances': [
              {
                'providerId': 'snap',
                'nativeId': 'cli-helper',
                'installationSource': 'snap',
                'installationScope': 'system',
              },
            ],
            'originalRecordIds': ['snap:system:cli-helper'],
            'desktopEntries': <Object?>[],
            'linkedPackageIdentityIds': <String>[],
            'linkedAppStreamComponentIds': <String>[],
            'metadata': {
              'name': 'CLI Helper',
              'version': '1.0.4',
              'channel': 'latest/stable',
            },
            'catalogReleases': {'releases': <Object?>[]},
            'capabilities': {
              'supportsFiles': 'unknown',
              'supportsUris': 'unknown',
              'requiresTerminal': 'unknown',
              'launchable': 'unknown',
            },
            'desktopMenuVisibility': 'unknown',
            'classification': 'cliApplication',
            'metadataConflicts': <Object?>[],
            'ambiguities': <Object?>[],
          },
        ],
        'packages': includePackages
            ? [
                {
                  'packageIdentityId': 'dpkg:system:coreutils:amd64',
                  'provenance': {
                    'providerId': 'dpkg',
                    'nativeId': 'coreutils',
                    'installationSource': 'dpkg',
                    'installationScope': 'system',
                  },
                  'packageName': 'coreutils',
                  'architecture': 'amd64',
                  'version': '9.4-3',
                  'summary': 'GNU core utilities',
                  'section': 'utils',
                  'installationState': 'installed',
                  'selectionState': 'install',
                  'installReason': 'explicit',
                  'classification': 'unknown',
                },
              ]
            : <Object?>[],
        'providerDiagnostics': [
          {'providerId': 'gio', 'status': 'succeeded', 'discoveredCount': 1},
          {
            'providerId': 'flatpak',
            'status': 'partiallyFailed',
            'discoveredCount': 1,
            'errorMessage': 'system installation unreadable',
          },
          {'providerId': 'rpm', 'status': 'unavailable', 'discoveredCount': 0},
        ],
        'ambiguities': [
          {
            'reason': 'ambiguous_file_ownership',
            'candidateIds': ['pkg-a', 'pkg-b'],
          },
        ],
      };

  final calls = <MethodCall>[];
  bool failDiscovery = false;

  setUp(() {
    calls.clear();
    failDiscovery = false;
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
          calls.add(call);
          if (call.method == 'discoverApps') {
            if (failDiscovery) {
              throw PlatformException(
                code: 'ERR_DISCOVERY',
                message: 'Simulated discovery failure',
              );
            }
            final args = call.arguments as Map<Object?, Object?>?;
            return sampleDiscoveryPayload(
              includePackages: args?['includePackages'] as bool? ?? false,
            );
          }
          return switch (call.method) {
            'getApp' => {
              ...editorLegacy,
              'categories': ['Utility'],
              'mimeTypes': ['text/plain'],
            },
            'getAppIcon' => null,
            'detectAppTechnologies' => ['libQt6Core.so.6'],
            'getDefaultAppForMimeType' ||
            'getDefaultAppForFile' => editorLegacy,
            'listAppsForMimeType' ||
            'listAppsForFile' => [editorLegacy, browserLegacy],
            'openApp' => true,
            _ => throw MissingPluginException(call.method),
          };
        });
  });

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  testWidgets(
    'loads dashboard via discoverApps, filters apps locally, inspects details, and launches',
    (tester) async {
      tester.view.physicalSize = const Size(1440, 900);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.reset);

      await tester.pumpWidget(const LinuxDeviceAppsExample());
      await tester.pumpAndSettle();

      // Single unified discovery request on startup with packages enabled.
      final discoverCalls = calls
          .where((c) => c.method == 'discoverApps')
          .toList();
      expect(discoverCalls, hasLength(1));
      expect(discoverCalls.single.arguments, {
        'includePackages': true,
        'includeAppStreamMetadata': true,
      });

      // Dashboard displays partial-failure banner and real stats.
      expect(find.textContaining('Partial discovery results'), findsOneWidget);
      expect(find.text('Applications'), findsWidgets);
      expect(find.text('Installed packages'), findsOneWidget);

      // Navigate to Applications without redundant discovery requests.
      await tester.tap(find.byIcon(Icons.apps_outlined));
      await tester.pumpAndSettle();
      expect(calls.where((c) => c.method == 'discoverApps'), hasLength(1));
      expect(find.text('Example Editor'), findsOneWidget);
      expect(find.text('CLI Helper'), findsOneWidget);

      // Instant local search filters without calling native discovery again.
      await tester.enterText(
        find.byKey(const ValueKey('apps_search_field')),
        'editor',
      );
      await tester.pumpAndSettle();
      expect(calls.where((c) => c.method == 'discoverApps'), hasLength(1));
      expect(find.text('CLI Helper'), findsNothing);
      expect(find.text('Example Editor'), findsOneWidget);

      // Select application in master-detail pane and inspect metadata,
      // separate catalog version, technologies, conflicts, and launch.
      await tester.tap(find.text('Example Editor'));
      await tester.pumpAndSettle();
      expect(find.text('2.1.0'), findsWidgets);
      expect(find.text('2.3.0'), findsWidgets);
      expect(find.textContaining('Lib: qtCore'), findsOneWidget);
      expect(find.textContaining('Field "summary"'), findsOneWidget);

      await tester.tap(find.text('Open app'));
      await tester.pumpAndSettle();
      expect(calls.last.method, 'openApp');
      expect(calls.last.arguments, editorDesktopId);
      expect(
        find.text('Launch requested for $editorDesktopId.'),
        findsOneWidget,
      );
    },
    variant: linux,
  );

  testWidgets(
    'browses installed packages, diagnostics, theme switcher, and handler lookup',
    (tester) async {
      tester.view.physicalSize = const Size(1440, 900);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.reset);

      await tester.pumpWidget(const LinuxDeviceAppsExample());
      await tester.pumpAndSettle();

      // Switch to Packages view and inspect package record.
      await tester.tap(find.byIcon(Icons.inventory_2_outlined));
      await tester.pumpAndSettle();
      expect(find.text('coreutils'), findsOneWidget);
      await tester.tap(find.text('coreutils'));
      await tester.pumpAndSettle();
      expect(find.text('dpkg:system:coreutils:amd64'), findsOneWidget);

      // Switch to Diagnostics view.
      await tester.tap(find.byIcon(Icons.monitor_heart_outlined));
      await tester.pumpAndSettle();
      expect(find.text('system installation unreadable'), findsOneWidget);
      expect(find.text('ambiguous_file_ownership'), findsOneWidget);

      // Switch to Handlers view and query MIME type & file path.
      await tester.tap(find.byIcon(Icons.find_in_page_outlined));
      await tester.pumpAndSettle();
      await tester.enterText(
        find.byKey(const ValueKey('handlers_input_field')),
        'text/plain',
      );
      await tester.tap(find.byTooltip('Look up handlers'));
      await tester.pumpAndSettle();
      expect(calls.map((c) => c.method), contains('listAppsForMimeType'));
      expect(find.text('Default'), findsOneWidget);
      expect(find.text('Editor'), findsOneWidget);
      expect(find.text('Browser'), findsOneWidget);

      await tester.enterText(
        find.byKey(const ValueKey('handlers_input_field')),
        '/tmp/report.txt',
      );
      await tester.tap(find.byTooltip('Look up handlers'));
      await tester.pumpAndSettle();
      expect(calls.map((c) => c.method), contains('listAppsForFile'));

      // Toggle dark theme via AppBar theme menu.
      await tester.tap(find.byTooltip('Theme mode'));
      await tester.pumpAndSettle();
      await tester.tap(find.text('Dark theme'));
      await tester.pumpAndSettle();
      final app = tester.widget<MaterialApp>(find.byType(MaterialApp));
      expect(app.themeMode, ThemeMode.dark);
    },
    variant: linux,
  );

  testWidgets('renders error state and recovers on retry', (tester) async {
    failDiscovery = true;
    await tester.pumpWidget(const LinuxDeviceAppsExample());
    await tester.pumpAndSettle();
    expect(find.text('Unable to complete discovery'), findsOneWidget);

    failDiscovery = false;
    await tester.tap(find.text('Retry'));
    await tester.pumpAndSettle();
    expect(find.text('Unable to complete discovery'), findsNothing);
    expect(find.text('Applications'), findsWidgets);
  }, variant: linux);
}
