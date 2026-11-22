# flutter_device_apps_linux

<p align="center">
<a href="https://pub.dev/packages/flutter_device_apps_linux"><img src="https://img.shields.io/pub/v/flutter_device_apps_linux.svg?color=0175C2" alt="Pub"></a>
<a href="https://pub.dev/packages/flutter_device_apps_linux/score"><img src="https://img.shields.io/pub/points/flutter_device_apps_linux" alt="flutter_device_apps_linux Pub points"></a>
<a href="https://github.com/okmsbun/flutter_device_apps"><img src="https://img.shields.io/github/stars/okmsbun/flutter_device_apps.svg?style=flat&logo=github&colorB=deeppink&label=stars" alt="GitHub stars for flutter_device_apps"></a>
<a href="https://opensource.org/licenses/MIT"><img src="https://img.shields.io/badge/license-MIT-purple.svg" alt="License: MIT"></a>
<a href="https://pub.dev/packages/flutter_device_apps"><img src="https://img.shields.io/badge/Main%20Package-orange.svg" alt="Main Package"></a>
</p>

A Flutter plugin to discover and open Linux desktop apps, get their icons and
metadata, find file handlers, and identify directly linked technologies.

## Requirements

Requires Linux desktop with GTK 3 and GIO, Flutter 3.44+, and Dart 3.12+.
Use Flutter's standard Linux build tooling.

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
| `getAppIcon` | Get the app icon as PNG bytes. |
| `openApp` | Request a launch through GIO and return whether it was accepted. |
| `getAppMetadata` | Read homepage, developer, license, and description from local AppStream MetaInfo. |
| `getInstallSourceInfo` | Read desktop metadata indicating Flatpak, Snap, AppImage, or `unknown`. |

## Linux setup

Discovery uses the desktop session's GIO and XDG data directories.
Available entries and metadata depend on session visibility and filesystem access.
See [Flutter's Linux setup](https://docs.flutter.dev/platform-integration/linux/setup)
and the [Linux development example](example/README.md).

## Community Standards

[Issues](https://github.com/okmsbun/flutter_device_apps_linux/issues) · [Contributing](CONTRIBUTING.md) · [Code of Conduct](CODE_OF_CONDUCT.md) · [Security Policy](SECURITY.md)

## License

[MIT](LICENSE) © 2026 okmsbun
