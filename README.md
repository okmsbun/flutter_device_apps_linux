# flutter_device_apps_linux

A Flutter plugin to list installed Linux desktop applications with their display names and desktop file IDs.

Requires Flutter 3.44+ and Dart 3.12+.

## Usage

```sh
flutter pub add flutter_device_apps_linux
```

```dart
import 'package:flutter/foundation.dart';
import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';

Future<void> showInstalledApps() async {
  for (final app in await FlutterDeviceAppsLinux.listApps()) {
    debugPrint('${app.appName} (${app.desktopFileId})');
  }
}
```

Call this after Flutter initializes. The list includes applications registered
with GIO that are visible in desktop menus.

[Flutter Device Apps](https://github.com/okmsbun/flutter_device_apps)
