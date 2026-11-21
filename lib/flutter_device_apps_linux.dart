import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

/// An application registered with the Linux desktop.
class LinuxAppInfo {
  const LinuxAppInfo({required this.desktopFileId, required this.appName});

  /// The desktop entry identifier, such as `org.gnome.Nautilus.desktop`.
  final String desktopFileId;

  /// The display name provided by the desktop entry.
  final String appName;
}

/// Discovers registered desktop applications on Linux.
abstract final class FlutterDeviceAppsLinux {
  static const _channel = MethodChannel('flutter_device_apps_linux/methods');

  /// Lists registered applications that GIO considers visible in desktop menus.
  static Future<List<LinuxAppInfo>> listApps() async {
    if (kIsWeb || defaultTargetPlatform != TargetPlatform.linux) {
      throw UnsupportedError('FlutterDeviceAppsLinux only supports Linux.');
    }

    final apps = await _channel.invokeListMethod<Map<Object?, Object?>>(
      'listApps',
    );
    return [
      for (final app in apps ?? const [])
        LinuxAppInfo(
          desktopFileId: app['desktopFileId'] as String,
          appName: app['appName'] as String,
        ),
    ];
  }
}
