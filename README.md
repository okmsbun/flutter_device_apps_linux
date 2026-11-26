# flutter_device_apps_linux

<p align="center">
<a href="https://pub.dev/packages/flutter_device_apps_linux"><img src="https://img.shields.io/pub/v/flutter_device_apps_linux.svg?color=0175C2" alt="Pub"></a>
<a href="https://pub.dev/packages/flutter_device_apps_linux/score"><img src="https://img.shields.io/pub/points/flutter_device_apps_linux" alt="flutter_device_apps_linux Pub points"></a>
<a href="https://github.com/okmsbun/flutter_device_apps"><img src="https://img.shields.io/github/stars/okmsbun/flutter_device_apps.svg?style=flat&logo=github&colorB=deeppink&label=stars" alt="GitHub stars for flutter_device_apps"></a>
<a href="https://opensource.org/licenses/MIT"><img src="https://img.shields.io/badge/license-MIT-purple.svg" alt="License: MIT"></a>
<a href="https://pub.dev/packages/flutter_device_apps"><img src="https://img.shields.io/badge/Main%20Package-orange.svg" alt="Main Package"></a>
</p>

A Flutter plugin to discover Linux applications across local desktop and
packaging sources (GIO, Flatpak, Snap, DPKG, RPM, Pacman, Nix, and AppStream),
open desktop apps, get their icons and metadata, find file handlers, and
identify directly linked ELF technologies.

## Requirements

Requires Linux desktop with GTK 3, GIO, and GLib (`gmodule-2.0`), Flutter
3.44+, and Dart 3.12+. Use Flutter's standard Linux build tooling. Optional
native libraries (`libflatpak`, `librpm`, `libalpm`, `libappstream`) are loaded
dynamically at runtime only when available and are never required to build or
run the plugin.

[API reference](https://pub.dev/documentation/flutter_device_apps_linux/latest/flutter_device_apps_linux/)
· [Guide to Linux desktop app discovery](doc/api-capabilities.md)

## Main package

`flutter_device_apps` 2.1.0+ re-exports this plugin alongside Android and macOS.
You can also add this package directly for Linux. [Main package](https://pub.dev/packages/flutter_device_apps) · [Main package README](https://github.com/okmsbun/flutter_device_apps#readme)

## Usage

Add the Linux package to your Flutter app.

```sh
flutter pub add flutter_device_apps_linux
```

```dart
import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
```

Call `WidgetsFlutterBinding.ensureInitialized()` if you use the API before
`runApp()`. The binding is already available in `initState()` and UI callbacks.

### List and search Linux desktop apps

```dart
final apps = await FlutterDeviceAppsLinux.listApps();
final matches = await FlutterDeviceAppsLinux.searchApps('image editor');
```

Both methods return desktop entries that GIO considers visible in menus.
Search results follow GIO's relevance order.

### Discover normalized applications across local Linux sources

```dart
final inventory = await FlutterDeviceAppsLinux.discoverApps();
for (final app in inventory.applications) {
  print(
    '${app.resolvedId}: ${app.metadata.name} '
    '(installed: ${app.metadata.version}, '
    'classification: ${app.classification.name})',
  );
}

// Check per-provider execution status and diagnostics:
for (final diag in inventory.providerDiagnostics) {
  print('${diag.providerId}: ${diag.status.name} (${diag.discoveredCount})');
}

// Optionally restrict providers, skip AppStream, or include raw packages:
final full = await FlutterDeviceAppsLinux.discoverApps(
  includePackages: true,
  includeAppStreamMetadata: true,
  providers: const ['gio', 'flatpak', 'dpkg'],
);
print('Installed packages: ${full.packages.length}');
```

`discoverApps()` queries available local read-only sources, correlates and
deduplicates verified installations, and returns an immutable
`LinuxDiscoveryResult`:

| Provider ID | Source | Runtime requirement |
| --- | --- | --- |
| `gio` | XDG `.desktop` entries | Built-in GIO (required) |
| `flatpak` | User and system Flatpak installations | Optional `libflatpak` via `GModule` |
| `snap` | Enabled Snap apps | Optional local `snapd` UNIX socket |
| `dpkg` | Local DPKG status and file ownership | Optional local `/var/lib/dpkg` |
| `rpm` | Local RPM database and file ownership | Optional `librpm` via `GModule` |
| `pacman` | Local Pacman database and file ownership | Optional `libalpm` via `GModule` |
| `nix` | Active local Nix profiles | Optional local Nix profile manifests |
| `appstream` | Local AppStream MetaInfo / catalogs | Optional `libappstream` via `GModule` |

Key immutable models returned by `discoverApps()`:

- `LinuxDiscoveryResult`: `applications`, optional `packages` (empty unless
  `includePackages: true`), `providerDiagnostics`, and top-level `ambiguities`.
- `LinuxDiscoveredApp`: `resolvedId`, `primaryProvenance`, `sourceProvenances`,
  `desktopEntries`, `metadata` (verified installed version), `catalogReleases`
  (separate upstream AppStream release history), tri-state `capabilities`,
  `desktopMenuVisibility`, `classification`, `metadataConflicts`, and
  `ambiguities`.
- `LinuxInstalledPackage`: raw system/profile package records kept strictly
  separate from confirmed applications.

Known limitations: missing optional libraries or inaccessible databases report
`LinuxDiscoveryProviderStatus.unavailable` or `failed` without aborting the
pass; desktop entries are never fabricated for desktop-less apps; classification
remains `unknown` without authoritative evidence; and `resolvedId` changes if an
RPM package transitions between single-version and simultaneous parallel
installations. See [Guide to Linux desktop app discovery](doc/api-capabilities.md)
for full details. Legacy `listApps()` remains GIO-only and unchanged.

### Get an app by desktop file ID

```dart
const id = 'org.gnome.Nautilus.desktop';
final app = await FlutterDeviceAppsLinux.getApp(id);
final metadata = await FlutterDeviceAppsLinux.getAppMetadata(id);
```

Use the `desktopFileId` from discovery to look up the exact desktop entry.
`getApp()` includes the desktop file path, description, categories, and declared
MIME types when available. `LinuxAppInfo` also exposes `supportsFiles`,
`supportsUris`, and `requiresTerminal`.

### Detect linked app technologies

```dart
final technologies = await FlutterDeviceAppsLinux.detectAppTechnologies(
  'org.videolan.VLC.desktop',
);
final libraries = technologies?.libraries;
```

| Category | Supported technologies |
| --- | --- |
| `frameworks` | Flutter, Qt |
| `uiToolkits` | Flutter, Qt Widgets, Qt Quick, GTK 3, GTK 4, libadwaita |
| `libraries` | Qt Core, SDL2, SDL3, WebKitGTK, GStreamer, libVLC, libmpv, OpenGL, Vulkan |

Matches come from direct ELF dependencies. An unavailable desktop entry returns
`null`. Script launchers and packaged wrappers can yield empty categories.

### Find apps that can open a file

```dart
const path = '/home/me/Documents/report.pdf';
final reader = await FlutterDeviceAppsLinux.getDefaultAppForFile(path);
final handlers = await FlutterDeviceAppsLinux.listAppsForFile(path);
final pdfHandlers = await FlutterDeviceAppsLinux.listAppsForMimeType(
  'application/pdf',
);
```

File lookups use an absolute local path. MIME lookups use the type directly.
Use `getDefaultAppForMimeType()` to read its default association.

## More APIs

All methods below belong to `FlutterDeviceAppsLinux`.

| Method | Purpose |
| --- | --- |
| `discoverApps` | Discover normalized applications across local Linux sources, with optional package inventory and provider diagnostics. |
| `getAppIcon` | Get the app icon as PNG bytes. |
| `openApp` | Request a launch through GIO and return whether it was accepted. |
| `getAppMetadata` | Read homepage, developer, license, and description from local AppStream MetaInfo. |
| `getInstallSourceInfo` | Read desktop metadata indicating Flatpak, Snap, AppImage, or `unknown`. |

## Linux setup

Desktop entry lookup uses the desktop session's GIO and XDG data directories.
`discoverApps()` additionally inspects available local package databases,
profiles, sockets, and optional shared libraries in read-only mode without
executing package manager CLIs or network requests. Available entries and
metadata depend on session visibility, installed providers, and filesystem
permissions. See [Flutter's Linux setup](https://docs.flutter.dev/platform-integration/linux/setup)
and the [Linux application explorer example](example/README.md) for a complete
Material 3 app showcasing `discoverApps()`, package inventory browsing, provider
diagnostics, launching, technology detection, and handler lookup.

## Community Standards

[Issues](https://github.com/okmsbun/flutter_device_apps_linux/issues) · [Contributing](CONTRIBUTING.md) · [Code of Conduct](CODE_OF_CONDUCT.md) · [Security Policy](SECURITY.md)

## License

[MIT](LICENSE) © 2026 okmsbun
