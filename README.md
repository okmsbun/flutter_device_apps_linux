# flutter_device_apps_linux

A Flutter plugin to discover, inspect, and open registered Linux desktop applications.

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

`searchApps(query)` returns matching visible desktop entries in GIO's relevance
order. Both discovery methods report whether an app accepts files or URIs and
whether its desktop entry requests a terminal.

## App details and actions

Pass the `desktopFileId` from `listApps()` to these methods.

| Method | Result |
| --- | --- |
| `getApp` | Desktop file path, description, categories, and declared MIME types when available. |
| `getAppIcon` | PNG bytes for the app's icon, or `null`. |
| `openApp` | Whether GIO accepted the launch request. |
| `getAppMetadata` | Homepage, developer, project license, and long description from matching local AppStream MetaInfo. |
| `getInstallSourceInfo` | Desktop metadata indicating Flatpak, Snap, AppImage, or `unknown`. |
| `detectAppTechnologies` | Frameworks, UI toolkits, and libraries linked directly by an accessible ELF executable. |

Use `getDefaultAppForFile(filePath)` or `listAppsForFile(filePath)` with an
absolute path to query the registered handlers for a local file's content type.
Use `getDefaultAppForMimeType(mimeType)` or `listAppsForMimeType(mimeType)` to
query the same associations by MIME type.

AppStream details come from accessible `.metainfo.xml` or legacy `.appdata.xml`
files. The desktop launchable ID links the MetaInfo to the application. Legacy
records can match by their component ID. Missing details are nullable.

Technology detection recognizes Flutter, Qt Core, Qt Widgets, Qt Quick, GTK,
libadwaita, SDL, WebKitGTK, GStreamer, libVLC, libmpv, OpenGL, and Vulkan from
direct ELF dependencies. Script launchers and sandboxed packaging may leave
the categories empty. Discovery and metadata reflect the session's GIO and
filesystem access.

[Flutter Device Apps](https://github.com/okmsbun/flutter_device_apps)
