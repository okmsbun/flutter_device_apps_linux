import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'src/appstream_stub.dart' if (dart.library.io) 'src/appstream.dart';

part 'linux_app_technologies.dart';

/// An application registered with the Linux desktop.
class LinuxAppInfo {
  const LinuxAppInfo({
    required this.desktopFileId,
    required this.appName,
    this.description,
    this.desktopFilePath,
    this.categories = const [],
    this.mimeTypes = const [],
    this.supportsFiles = false,
    this.supportsUris = false,
    this.requiresTerminal = false,
  });

  factory LinuxAppInfo._fromMap(Map<Object?, Object?> app) => LinuxAppInfo(
    desktopFileId: app['desktopFileId'] as String,
    appName: app['appName'] as String,
    description: app['description'] as String?,
    desktopFilePath: app['desktopFilePath'] as String?,
    categories: List<String>.unmodifiable(
      app['categories'] as List<Object?>? ?? const [],
    ),
    mimeTypes: List<String>.unmodifiable(
      app['mimeTypes'] as List<Object?>? ?? const [],
    ),
    supportsFiles: app['supportsFiles'] as bool? ?? false,
    supportsUris: app['supportsUris'] as bool? ?? false,
    requiresTerminal: app['requiresTerminal'] as bool? ?? false,
  );

  /// The desktop entry identifier, such as `org.gnome.Nautilus.desktop`.
  final String desktopFileId;

  /// The display name provided by the desktop entry.
  final String appName;

  /// The localized description, when detail metadata is available.
  final String? description;

  /// The desktop entry file path, when detail metadata is available.
  final String? desktopFilePath;

  /// The declared desktop categories, when detail metadata is available.
  final List<String> categories;

  /// The declared MIME types, when detail metadata is available.
  final List<String> mimeTypes;

  /// Whether GIO reports that the desktop entry accepts local files.
  final bool supportsFiles;

  /// Whether GIO reports that the desktop entry accepts URIs.
  final bool supportsUris;

  /// Whether the desktop entry requests a terminal for launch.
  final bool requiresTerminal;
}

/// Optional details from an installed application's AppStream MetaInfo file.
class LinuxAppMetadata {
  const LinuxAppMetadata({
    this.homepage,
    this.developer,
    this.license,
    this.description,
  });

  final Uri? homepage;
  final String? developer;

  /// The declared project license.
  final String? license;

  /// The localized long description as plain text.
  final String? description;
}

/// The packaging format indicated by an application's desktop entry.
enum LinuxAppInstallSource { flatpak, snap, appImage, unknown }

/// Packaging metadata declared in a desktop entry.
class LinuxAppInstallSourceInfo {
  const LinuxAppInstallSourceInfo({
    required this.source,
    this.packageId,
    this.versionName,
  });

  final LinuxAppInstallSource source;

  /// The declared Flatpak application ID or Snap instance name.
  final String? packageId;

  /// The declared AppImage version, when available.
  final String? versionName;
}

/// Discovers and opens registered desktop applications on Linux.
abstract final class FlutterDeviceAppsLinux {
  static const _channel = MethodChannel('flutter_device_apps_linux/methods');

  /// Lists registered applications that GIO considers visible in desktop menus.
  static Future<List<LinuxAppInfo>> listApps() => _listApps('listApps');

  /// Searches visible desktop applications in GIO's relevance order.
  ///
  /// Empty or whitespace-only queries return an empty list.
  static Future<List<LinuxAppInfo>> searchApps(String query) async {
    _checkPlatform();
    if (query.contains('\u0000')) {
      throw ArgumentError.value(query, 'query', 'Expected a search string');
    }
    final search = query.trim();
    if (search.isEmpty) return [];
    return _listApps('searchApps', search);
  }

  /// Looks up a registered app by its desktop file ID, independently of menu
  /// visibility. Returns null if GIO cannot resolve the ID.
  ///
  /// An invalid desktop file ID throws [ArgumentError].
  static Future<LinuxAppInfo?> getApp(String desktopFileId) async =>
      _getApp('getApp', _desktopId(desktopFileId));

  /// Returns a 64-pixel PNG icon, or null if no icon can be loaded.
  static Future<Uint8List?> getAppIcon(String desktopFileId) async =>
      _channel.invokeMethod<Uint8List>('getAppIcon', _desktopId(desktopFileId));

  /// Requests an app launch through GIO. Returns false if the ID is unavailable.
  ///
  /// True indicates that GIO accepted the launch request. Launch errors throw
  /// [PlatformException] with code `ERR_OPEN`.
  static Future<bool> openApp(String desktopFileId) async =>
      await _channel.invokeMethod<bool>('openApp', _desktopId(desktopFileId)) ??
      false;

  /// Returns the default handler for an absolute local file path, if available.
  static Future<LinuxAppInfo?> getDefaultAppForFile(String filePath) async =>
      _getApp('getDefaultAppForFile', _filePath(filePath));

  /// Lists registered handlers for an absolute local file path.
  ///
  /// A missing file or missing association yields an empty list. Other lookup
  /// errors throw [PlatformException] with code `ERR_FILE_HANDLERS`.
  static Future<List<LinuxAppInfo>> listAppsForFile(String filePath) async =>
      _listApps('listAppsForFile', _filePath(filePath));

  /// Gets the default handler for a MIME type such as `application/pdf`.
  /// Returns null when there is no default handler.
  static Future<LinuxAppInfo?> getDefaultAppForMimeType(
    String mimeType,
  ) async => _getApp('getDefaultAppForMimeType', _mimeType(mimeType));

  /// Lists registered handlers for a MIME type, or an empty list if none match.
  static Future<List<LinuxAppInfo>> listAppsForMimeType(
    String mimeType,
  ) async => _listApps('listAppsForMimeType', _mimeType(mimeType));

  /// Reads local AppStream MetaInfo associated with a desktop file ID.
  ///
  /// Returns null when the desktop entry or readable metadata is unavailable.
  static Future<LinuxAppMetadata?> getAppMetadata(String desktopFileId) async {
    final id = _desktopId(desktopFileId);
    final locations = await _channel.invokeMapMethod<Object?, Object?>(
      'getAppMetadata',
      id,
    );
    if (locations == null) return null;
    final fields = await readAppStream(
      desktopFileId: id,
      desktopFilePath: locations['desktopFilePath'] as String?,
      dataDirectories: (locations['dataDirectories'] as List).cast<String>(),
      languages: (locations['languages'] as List).cast<String>(),
    );
    if (fields == null) return null;
    final homepage = fields['homepage'];
    return LinuxAppMetadata(
      homepage: homepage == null ? null : Uri.tryParse(homepage),
      developer: fields['developer'],
      license: fields['license'],
      description: fields['description'],
    );
  }

  /// Returns desktop packaging metadata, or null if the app is unavailable.
  static Future<LinuxAppInstallSourceInfo?> getInstallSourceInfo(
    String desktopFileId,
  ) async {
    final result = await _channel.invokeMapMethod<Object?, Object?>(
      'getInstallSourceInfo',
      _desktopId(desktopFileId),
    );
    if (result == null) return null;
    return LinuxAppInstallSourceInfo(
      source: LinuxAppInstallSource.values.firstWhere(
        (source) => source.name == result['source'],
        orElse: () => LinuxAppInstallSource.unknown,
      ),
      packageId: result['packageId'] as String?,
      versionName: result['versionName'] as String?,
    );
  }

  /// Finds supported technologies linked directly to an app's ELF executable.
  ///
  /// Returns null for an unavailable desktop entry. Empty categories indicate
  /// that no supported direct dependency was found.
  static Future<LinuxAppTechnologies?> detectAppTechnologies(
    String desktopFileId,
  ) async {
    final dependencies = await _channel.invokeListMethod<String>(
      'detectAppTechnologies',
      _desktopId(desktopFileId),
    );
    return dependencies == null
        ? null
        : LinuxAppTechnologies._fromDependencies(dependencies);
  }

  static Future<List<LinuxAppInfo>> _listApps(
    String method, [
    String? argument,
  ]) async {
    _checkPlatform();
    final apps = await _channel.invokeListMethod<Map<Object?, Object?>>(
      method,
      argument,
    );
    return [for (final app in apps ?? const []) LinuxAppInfo._fromMap(app)];
  }

  static Future<LinuxAppInfo?> _getApp(String method, String argument) async {
    final app = await _channel.invokeMapMethod<Object?, Object?>(
      method,
      argument,
    );
    return app == null ? null : LinuxAppInfo._fromMap(app);
  }

  static String _desktopId(String value) {
    _checkPlatform();
    if (!value.endsWith('.desktop') ||
        value.contains('/') ||
        value.contains('\u0000')) {
      throw ArgumentError.value(
        value,
        'desktopFileId',
        'Expected a desktop ID',
      );
    }
    return value;
  }

  static String _filePath(String value) {
    _checkPlatform();
    if (!value.startsWith('/') || value.contains('\u0000')) {
      throw ArgumentError.value(value, 'filePath', 'Expected an absolute path');
    }
    return value;
  }

  static final _mimeTypePattern = RegExp(
    r'^[a-z0-9!#$&^_.+-]+/[a-z0-9!#$&^_.+-]+$',
  );

  static String _mimeType(String value) {
    _checkPlatform();
    final type = value.trim().toLowerCase();
    if (!_mimeTypePattern.hasMatch(type)) {
      throw ArgumentError.value(value, 'mimeType', 'Expected a MIME type');
    }
    return type;
  }

  static void _checkPlatform() {
    if (kIsWeb || defaultTargetPlatform != TargetPlatform.linux) {
      throw UnsupportedError('FlutterDeviceAppsLinux only supports Linux.');
    }
  }
}
