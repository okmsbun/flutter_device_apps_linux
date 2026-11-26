import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('flutter_device_apps_linux/methods');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

  setUp(() => debugDefaultTargetPlatformOverride = TargetPlatform.linux);
  tearDown(() {
    messenger.setMockMethodCallHandler(channel, null);
    debugDefaultTargetPlatformOverride = null;
  });

  test('decodes full unified discovery payload and preserves separation', () async {
    Object? capturedArgs;
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'discoverApps');
      capturedArgs = call.arguments;
      return <String, Object?>{
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
                'nativeId': 'org.example.Editor.desktop',
                'installationSource': 'unknown',
                'installationScope': 'unknown',
              },
            ],
            'originalRecordIds': [
              'flatpak:user:org.example.Editor:user/stable/x86_64',
              'gio:unknown:org.example.Editor.desktop',
            ],
            'desktopEntries': [
              {
                'desktopFileId': 'org.example.Editor.desktop',
                'desktopFilePath':
                    '/home/me/.local/share/flatpak/exports/share/applications/org.example.Editor.desktop',
                'displayName': 'Example Editor',
                'desktopMenuVisibility': 'visible',
                'capabilities': {
                  'supportsFiles': 'supported',
                  'supportsUris': 'unsupported',
                  'requiresTerminal': 'unsupported',
                  'launchable': 'supported',
                },
                'providerId': 'gio',
                'originalApplicationId': 'gio:unknown:org.example.Editor.desktop',
              },
            ],
            'linkedPackageIdentityIds': <String>[],
            'linkedAppStreamComponentIds': ['org.example.Editor'],
            'metadata': {
              'name': 'Example Editor',
              'version': '2.1.0',
              'revision': null,
              'channel': null,
              'summary': 'Edit files locally',
              'description': 'Full text editor.',
              'executable': 'editor',
              'homepage': 'https://example.org/editor',
              'license': 'MIT',
              'iconReference': 'org.example.Editor',
              'architecture': 'x86_64',
              'branch': 'stable',
            },
            'catalogReleases': {
              'latestVersion': '2.3.0',
              'latestDate': '2026-03-01',
              'releases': [
                {
                  'version': '2.3.0',
                  'date': '2026-03-01',
                  'timestamp': 1772323200,
                  'urgency': 'medium',
                },
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
                'chosenValue': 'Edit files locally',
                'conflictingProviderId': 'gio',
                'conflictingValue': 'Legacy Editor Summary',
              },
            ],
            'ambiguities': [
              {
                'reason': 'multiple_candidates',
                'candidateIds': ['cand-a', 'cand-b'],
              },
            ],
          },
          // Separate Snap installation of the same logical tool without desktop entry
          {
            'resolvedId': 'snap:system:editor_beta',
            'primaryProvenance': {
              'providerId': 'snap',
              'nativeId': 'editor_beta',
              'installationSource': 'snap',
              'installationScope': 'system',
            },
            'sourceProvenances': [
              {
                'providerId': 'snap',
                'nativeId': 'editor_beta',
                'installationSource': 'snap',
                'installationScope': 'system',
              },
            ],
            'originalRecordIds': ['snap:system:editor_beta'],
            'desktopEntries': <Object?>[],
            'linkedPackageIdentityIds': <String>[],
            'linkedAppStreamComponentIds': <String>[],
            'metadata': {
              'name': 'editor_beta',
              'version': '3.0.0-beta',
              'revision': '42',
              'channel': 'latest/beta',
            },
            'catalogReleases': {'releases': <Object?>[]},
            'capabilities': {
              'supportsFiles': 'unknown',
              'supportsUris': 'unknown',
              'requiresTerminal': 'unknown',
              'launchable': 'unknown',
            },
            'desktopMenuVisibility': 'unknown',
            'classification': 'unknown',
            'metadataConflicts': <Object?>[],
            'ambiguities': <Object?>[],
          },
        ],
        'packages': [
          {
            'packageIdentityId': 'rpm:system:kernel-core:x86_64',
            'installationIdentityId':
                'rpm:system:kernel-core:x86_64:0:6.8.1-1.fc40',
            'provenance': {
              'providerId': 'rpm',
              'nativeId': 'kernel-core',
              'installationSource': 'rpm',
              'installationScope': 'system',
            },
            'packageName': 'kernel-core',
            'architecture': 'x86_64',
            'epoch': '0',
            'version': '6.8.1',
            'release': '1.fc40',
            'summary': 'The Linux kernel',
            'description': 'Core kernel package.',
            'packageGroup': 'Unspecified',
            'groups': ['base'],
            'license': 'GPL-2.0-only',
            'licenses': ['GPL-2.0-only'],
            'homepage': 'https://www.kernel.org/',
            'vendor': 'Fedora Project',
            'outputs': <String>[],
            'storePaths': <String>[],
            'installPriority': 5,
            'installationState': 'installed',
            'selectionState': 'install',
            'installReason': 'explicit',
            'classification': 'unknown',
          },
        ],
        'providerDiagnostics': [
          {
            'providerId': 'gio',
            'status': 'succeeded',
            'discoveredCount': 1,
          },
          {
            'providerId': 'flatpak',
            'status': 'partiallyFailed',
            'discoveredCount': 1,
            'errorMessage': 'system installation unreadable',
          },
          {
            'providerId': 'pacman',
            'status': 'unavailable',
            'discoveredCount': 0,
          },
        ],
        'ambiguities': [
          {
            'reason': 'ambiguous_file_ownership',
            'candidateIds': ['pkg-1', 'pkg-2'],
          },
        ],
      };
    });

    final result = await FlutterDeviceAppsLinux.discoverApps(
      includePackages: true,
      includeAppStreamMetadata: true,
      providers: const ['gio', 'flatpak', 'snap', 'rpm'],
    );

    expect(capturedArgs, {
      'includePackages': true,
      'includeAppStreamMetadata': true,
      'providers': ['gio', 'flatpak', 'snap', 'rpm'],
    });

    // Separate installations preserved; no fabricated desktop IDs.
    expect(result.applications, hasLength(2));
    final flatpakApp = result.applications[0];
    expect(
      flatpakApp.resolvedId,
      'flatpak:user:org.example.Editor:user/stable/x86_64',
    );
    expect(flatpakApp.primaryProvenance.providerId, 'flatpak');
    expect(
      flatpakApp.primaryProvenance.installationSource,
      LinuxInstallationSource.flatpak,
    );
    expect(
      flatpakApp.primaryProvenance.installationScope,
      LinuxInstallationScope.user,
    );
    expect(flatpakApp.sourceProvenances, hasLength(2));
    expect(flatpakApp.originalRecordIds, hasLength(2));
    expect(flatpakApp.desktopEntries, hasLength(1));
    expect(
      flatpakApp.desktopEntries.single.desktopFileId,
      'org.example.Editor.desktop',
    );
    expect(
      flatpakApp.desktopEntries.single.desktopMenuVisibility,
      LinuxDesktopMenuVisibility.visible,
    );
    expect(
      flatpakApp.desktopEntries.single.capabilities.supportsFiles,
      LinuxCapabilityState.supported,
    );
    expect(flatpakApp.linkedAppStreamComponentIds, ['org.example.Editor']);

    // Installed version vs catalog release version remain strictly separate.
    expect(flatpakApp.metadata.version, '2.1.0');
    expect(flatpakApp.metadata.revision, isNull);
    expect(flatpakApp.metadata.channel, isNull);
    expect(flatpakApp.metadata.architecture, 'x86_64');
    expect(flatpakApp.metadata.branch, 'stable');
    expect(flatpakApp.catalogReleases.latestVersion, '2.3.0');
    expect(flatpakApp.catalogReleases.latestDate, '2026-03-01');
    expect(flatpakApp.catalogReleases.releases.single.timestamp, 1772323200);
    expect(flatpakApp.catalogReleases.releases.single.urgency, 'medium');
    expect(flatpakApp.classification, LinuxAppClassification.guiApplication);
    expect(flatpakApp.metadataConflicts.single.fieldName, 'summary');
    expect(flatpakApp.metadataConflicts.single.chosenProviderId, 'appstream');
    expect(flatpakApp.ambiguities.single.candidateIds, ['cand-a', 'cand-b']);

    // Desktop-less Snap instance stays distinct with empty desktopEntries.
    final snapApp = result.applications[1];
    expect(snapApp.resolvedId, 'snap:system:editor_beta');
    expect(snapApp.desktopEntries, isEmpty);
    expect(snapApp.metadata.revision, '42');
    expect(snapApp.metadata.channel, 'latest/beta');
    expect(snapApp.metadata.homepage, isNull);
    expect(snapApp.capabilities.launchable, LinuxCapabilityState.unknown);
    expect(snapApp.desktopMenuVisibility, LinuxDesktopMenuVisibility.unknown);
    expect(snapApp.classification, LinuxAppClassification.unknown);

    // Packages stay strictly separate from applications.
    expect(result.packages, hasLength(1));
    final pkg = result.packages.single;
    expect(pkg.packageIdentityId, 'rpm:system:kernel-core:x86_64');
    expect(
      pkg.installationIdentityId,
      'rpm:system:kernel-core:x86_64:0:6.8.1-1.fc40',
    );
    expect(pkg.provenance.installationSource, LinuxInstallationSource.rpm);
    expect(pkg.epoch, '0');
    expect(pkg.version, '6.8.1');
    expect(pkg.release, '1.fc40');
    expect(pkg.installPriority, 5);
    expect(pkg.installationState, LinuxPackageInstallationState.installed);
    expect(pkg.selectionState, LinuxPackageSelectionState.install);
    expect(pkg.installReason, LinuxPackageInstallReason.explicit);
    expect(pkg.classification, LinuxAppClassification.unknown);
    expect(pkg.profilePath, isNull);

    // Provider diagnostics and top-level ambiguities.
    expect(result.providerDiagnostics, hasLength(3));
    expect(
      result.providerDiagnostics[0].status,
      LinuxDiscoveryProviderStatus.succeeded,
    );
    expect(result.providerDiagnostics[0].errorMessage, isNull);
    expect(
      result.providerDiagnostics[1].status,
      LinuxDiscoveryProviderStatus.partiallyFailed,
    );
    expect(
      result.providerDiagnostics[1].errorMessage,
      'system installation unreadable',
    );
    expect(
      result.providerDiagnostics[2].status,
      LinuxDiscoveryProviderStatus.unavailable,
    );
    expect(result.ambiguities.single.reason, 'ambiguous_file_ownership');

    // All returned lists are immutable.
    expect(() => result.applications.add(snapApp), throwsUnsupportedError);
    expect(() => result.packages.add(pkg), throwsUnsupportedError);
    expect(
      () => result.providerDiagnostics.clear(),
      throwsUnsupportedError,
    );
    expect(() => result.ambiguities.clear(), throwsUnsupportedError);
    expect(
      () => flatpakApp.sourceProvenances.clear(),
      throwsUnsupportedError,
    );
    expect(
      () => flatpakApp.originalRecordIds.clear(),
      throwsUnsupportedError,
    );
    expect(() => flatpakApp.desktopEntries.clear(), throwsUnsupportedError);
    expect(
      () => flatpakApp.linkedAppStreamComponentIds.clear(),
      throwsUnsupportedError,
    );
    expect(
      () => flatpakApp.catalogReleases.releases.clear(),
      throwsUnsupportedError,
    );
    expect(
      () => flatpakApp.metadataConflicts.clear(),
      throwsUnsupportedError,
    );
    expect(() => pkg.groups.clear(), throwsUnsupportedError);
    expect(() => pkg.licenses.clear(), throwsUnsupportedError);
  });

  test('defaults omit providers and keep packages excluded', () async {
    Object? capturedArgs;
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'discoverApps');
      capturedArgs = call.arguments;
      return <String, Object?>{
        'applications': <Object?>[],
        'packages': <Object?>[],
        'providerDiagnostics': <Object?>[],
        'ambiguities': <Object?>[],
      };
    });

    final result = await FlutterDeviceAppsLinux.discoverApps();
    expect(capturedArgs, {
      'includePackages': false,
      'includeAppStreamMetadata': true,
    });
    expect(result.applications, isEmpty);
    expect(result.packages, isEmpty);
  });

  test('handles null response and unknown wire enum values conservatively', () async {
    messenger.setMockMethodCallHandler(channel, (call) async => null);
    final empty = await FlutterDeviceAppsLinux.discoverApps();
    expect(empty.applications, isEmpty);
    expect(empty.packages, isEmpty);
    expect(empty.providerDiagnostics, isEmpty);
    expect(empty.ambiguities, isEmpty);

    messenger.setMockMethodCallHandler(channel, (call) async {
      return <String, Object?>{
        'applications': [
          {
            'resolvedId': 'future:x',
            'primaryProvenance': {
              'providerId': 'future',
              'nativeId': 'x',
              'installationSource': 'futureSource',
              'installationScope': 'futureScope',
            },
            'capabilities': {
              'supportsFiles': 'maybe',
              'supportsUris': 'maybe',
              'requiresTerminal': 'maybe',
              'launchable': 'maybe',
            },
            'desktopMenuVisibility': 'sometimes',
            'classification': 'futureKind',
          },
        ],
        'packages': [
          {
            'packageIdentityId': 'future:pkg',
            'packageName': 'pkg',
            'installationState': 'futureState',
            'selectionState': 'futureSelection',
            'installReason': 'futureReason',
          },
        ],
        'providerDiagnostics': [
          {
            'providerId': 'future',
            'status': 'futureStatus',
          },
        ],
      };
    });

    final decoded = await FlutterDeviceAppsLinux.discoverApps(
      includePackages: true,
    );
    final app = decoded.applications.single;
    expect(
      app.primaryProvenance.installationSource,
      LinuxInstallationSource.unknown,
    );
    expect(
      app.primaryProvenance.installationScope,
      LinuxInstallationScope.unknown,
    );
    expect(app.capabilities.supportsFiles, LinuxCapabilityState.unknown);
    expect(app.desktopMenuVisibility, LinuxDesktopMenuVisibility.unknown);
    expect(app.classification, LinuxAppClassification.unknown);
    expect(app.metadata.name, isNull);
    expect(app.metadata.version, isNull);

    final pkg = decoded.packages.single;
    expect(pkg.installationState, LinuxPackageInstallationState.unknown);
    expect(pkg.selectionState, LinuxPackageSelectionState.unknown);
    expect(pkg.installReason, LinuxPackageInstallReason.unknown);
    expect(
      decoded.providerDiagnostics.single.status,
      LinuxDiscoveryProviderStatus.failed,
    );
  });

  test('validates arguments, platform, and preserves legacy listApps', () async {
    for (final invalid in [
      const <String>[],
      const [''],
      const ['gio', ''],
      const ['gio\u0000bad'],
    ]) {
      await expectLater(
        FlutterDeviceAppsLinux.discoverApps(providers: invalid),
        throwsArgumentError,
      );
    }

    debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
    await expectLater(
      FlutterDeviceAppsLinux.discoverApps(),
      throwsUnsupportedError,
    );

    debugDefaultTargetPlatformOverride = TargetPlatform.linux;
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'listApps');
      expect(call.arguments, isNull);
      return [
        {
          'desktopFileId': 'org.example.Legacy.desktop',
          'appName': 'Legacy',
          'supportsFiles': true,
          'supportsUris': false,
          'requiresTerminal': false,
        },
      ];
    });
    final legacy = await FlutterDeviceAppsLinux.listApps();
    expect(legacy.single.desktopFileId, 'org.example.Legacy.desktop');
    expect(legacy.single.appName, 'Legacy');
  });
}
