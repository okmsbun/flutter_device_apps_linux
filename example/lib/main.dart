import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
import 'package:material_ui/material_ui.dart';

import 'src/explorer_controller.dart';
import 'src/explorer_shell.dart';

/// Linux desktop application explorer built on
/// [FlutterDeviceAppsLinux.discoverApps].
///
/// Demonstrates unified multi-source discovery (GIO, Flatpak, Snap, DPKG, RPM,
/// Pacman, Nix, and local AppStream metadata), separate installed package
/// inventory, provider diagnostics, desktop launching, ELF technology
/// detection, and file/MIME handler lookup.
///
/// Quick standalone call:
/// ```dart
/// final result = await FlutterDeviceAppsLinux.discoverApps(
///   includePackages: true,
/// );
/// for (final app in result.applications) {
///   debugPrint('${app.resolvedId}: ${app.metadata.name}');
/// }
/// ```
void main() => runApp(const LinuxDeviceAppsExample());

class LinuxDeviceAppsExample extends StatefulWidget {
  const LinuxDeviceAppsExample({super.key, this.controller});

  final DiscoveryExplorerController? controller;

  @override
  State<LinuxDeviceAppsExample> createState() => _LinuxDeviceAppsExampleState();
}

class _LinuxDeviceAppsExampleState extends State<LinuxDeviceAppsExample> {
  static const _seedColor = Color(0xFF006A6A);

  late final DiscoveryExplorerController _controller;
  late final bool _ownsController;
  ThemeMode _themeMode = ThemeMode.system;

  @override
  void initState() {
    super.initState();
    _ownsController = widget.controller == null;
    _controller =
        widget.controller ??
        DiscoveryExplorerController(
          loader:
              ({required includePackages, required includeAppStreamMetadata}) =>
                  FlutterDeviceAppsLinux.discoverApps(
                    includePackages: includePackages,
                    includeAppStreamMetadata: includeAppStreamMetadata,
                  ),
        );
  }

  @override
  void dispose() {
    if (_ownsController) {
      _controller.dispose();
    }
    super.dispose();
  }

  static ThemeData _buildTheme(Brightness brightness) {
    final scheme = ColorScheme.fromSeed(
      seedColor: _seedColor,
      brightness: brightness,
    );
    return ThemeData(
      useMaterial3: true,
      colorScheme: scheme,
      cardTheme: CardThemeData(
        elevation: 0,
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(14),
          side: BorderSide(color: scheme.outlineVariant),
        ),
      ),
      inputDecorationTheme: InputDecorationTheme(
        isDense: true,
        filled: true,
        fillColor: scheme.surfaceContainerLow,
        border: OutlineInputBorder(borderRadius: BorderRadius.circular(12)),
      ),
      chipTheme: ChipThemeData(
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Flutter Device Apps Linux',
      debugShowCheckedModeBanner: false,
      themeMode: _themeMode,
      theme: _buildTheme(Brightness.light),
      darkTheme: _buildTheme(Brightness.dark),
      home: LinuxExplorerShell(
        controller: _controller,
        themeMode: _themeMode,
        onThemeModeChanged: (mode) => setState(() => _themeMode = mode),
      ),
    );
  }
}
