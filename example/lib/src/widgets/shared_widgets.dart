import 'dart:typed_data';

import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
import 'package:material_ui/material_ui.dart';

import '../explorer_controller.dart';

String formatSource(LinuxInstallationSource source) => switch (source) {
  LinuxInstallationSource.flatpak => 'Flatpak',
  LinuxInstallationSource.snap => 'Snap',
  LinuxInstallationSource.appImage => 'AppImage',
  LinuxInstallationSource.dpkg => 'DPKG',
  LinuxInstallationSource.rpm => 'RPM',
  LinuxInstallationSource.pacman => 'Pacman',
  LinuxInstallationSource.nix => 'Nix',
  LinuxInstallationSource.unknown => 'Unknown',
};

String formatScope(LinuxInstallationScope scope) => switch (scope) {
  LinuxInstallationScope.system => 'System',
  LinuxInstallationScope.user => 'User',
  LinuxInstallationScope.other => 'Other',
  LinuxInstallationScope.unknown => 'Unknown',
};

String formatClassification(LinuxAppClassification kind) => switch (kind) {
  LinuxAppClassification.guiApplication => 'GUI App',
  LinuxAppClassification.cliApplication => 'CLI App',
  LinuxAppClassification.service => 'Service',
  LinuxAppClassification.library => 'Library',
  LinuxAppClassification.runtime => 'Runtime',
  LinuxAppClassification.unknown => 'Unknown',
};

String formatVisibility(LinuxDesktopMenuVisibility visibility) =>
    switch (visibility) {
      LinuxDesktopMenuVisibility.visible => 'Visible in menu',
      LinuxDesktopMenuVisibility.notVisible => 'Hidden from menu',
      LinuxDesktopMenuVisibility.unknown => 'Unknown',
    };

String formatCapability(LinuxCapabilityState state) => switch (state) {
  LinuxCapabilityState.supported => 'Supported',
  LinuxCapabilityState.unsupported => 'Unsupported',
  LinuxCapabilityState.unknown => 'Unknown',
};

String formatProviderStatus(LinuxDiscoveryProviderStatus status) =>
    switch (status) {
      LinuxDiscoveryProviderStatus.succeeded => 'Succeeded',
      LinuxDiscoveryProviderStatus.partiallyFailed => 'Partial failure',
      LinuxDiscoveryProviderStatus.unavailable => 'Unavailable',
      LinuxDiscoveryProviderStatus.failed => 'Failed',
    };

String formatPackageInstallState(LinuxPackageInstallationState state) =>
    switch (state) {
      LinuxPackageInstallationState.installed => 'Installed',
      LinuxPackageInstallationState.triggersAwaited => 'Triggers awaited',
      LinuxPackageInstallationState.triggersPending => 'Triggers pending',
      LinuxPackageInstallationState.unknown => 'Unknown',
    };

String formatPackageSelectionState(LinuxPackageSelectionState state) =>
    switch (state) {
      LinuxPackageSelectionState.install => 'Install',
      LinuxPackageSelectionState.hold => 'Hold',
      LinuxPackageSelectionState.deinstall => 'Deinstall',
      LinuxPackageSelectionState.purge => 'Purge',
      LinuxPackageSelectionState.unknown => 'Unknown',
    };

String formatPackageInstallReason(LinuxPackageInstallReason reason) =>
    switch (reason) {
      LinuxPackageInstallReason.explicit => 'Explicit',
      LinuxPackageInstallReason.dependency => 'Dependency',
      LinuxPackageInstallReason.unknown => 'Unknown',
    };

IconData iconForClassification(LinuxAppClassification kind) => switch (kind) {
  LinuxAppClassification.guiApplication => Icons.window_outlined,
  LinuxAppClassification.cliApplication => Icons.terminal_outlined,
  LinuxAppClassification.service => Icons.dns_outlined,
  LinuxAppClassification.library => Icons.layers_outlined,
  LinuxAppClassification.runtime => Icons.memory_outlined,
  LinuxAppClassification.unknown => Icons.apps_outlined,
};

IconData iconForSource(LinuxInstallationSource source) => switch (source) {
  LinuxInstallationSource.flatpak => Icons.inventory_2_outlined,
  LinuxInstallationSource.snap => Icons.widgets_outlined,
  LinuxInstallationSource.appImage => Icons.archive_outlined,
  LinuxInstallationSource.dpkg => Icons.all_inbox_outlined,
  LinuxInstallationSource.rpm => Icons.redeem_outlined,
  LinuxInstallationSource.pacman => Icons.extension_outlined,
  LinuxInstallationSource.nix => Icons.ac_unit_outlined,
  LinuxInstallationSource.unknown => Icons.desktop_windows_outlined,
};

Color statusBackground(
  ColorScheme scheme,
  LinuxDiscoveryProviderStatus status,
) => switch (status) {
  LinuxDiscoveryProviderStatus.succeeded => scheme.primaryContainer,
  LinuxDiscoveryProviderStatus.partiallyFailed => scheme.tertiaryContainer,
  LinuxDiscoveryProviderStatus.unavailable => scheme.surfaceContainerHighest,
  LinuxDiscoveryProviderStatus.failed => scheme.errorContainer,
};

Color statusForeground(
  ColorScheme scheme,
  LinuxDiscoveryProviderStatus status,
) => switch (status) {
  LinuxDiscoveryProviderStatus.succeeded => scheme.onPrimaryContainer,
  LinuxDiscoveryProviderStatus.partiallyFailed => scheme.onTertiaryContainer,
  LinuxDiscoveryProviderStatus.unavailable => scheme.onSurfaceVariant,
  LinuxDiscoveryProviderStatus.failed => scheme.onErrorContainer,
};

/// Lazily loads and caches a desktop entry's PNG icon with a crisp fallback.
class AppIconAvatar extends StatefulWidget {
  const AppIconAvatar({
    super.key,
    required this.controller,
    required this.desktopFileId,
    required this.fallbackIcon,
    this.size = 40,
  });

  final DiscoveryExplorerController controller;
  final String? desktopFileId;
  final IconData fallbackIcon;
  final double size;

  @override
  State<AppIconAvatar> createState() => _AppIconAvatarState();
}

class _AppIconAvatarState extends State<AppIconAvatar> {
  Future<Uint8List?>? _future;

  @override
  void initState() {
    super.initState();
    _initIcon();
  }

  @override
  void didUpdateWidget(AppIconAvatar oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.desktopFileId != widget.desktopFileId ||
        oldWidget.controller != widget.controller) {
      _initIcon();
    }
  }

  void _initIcon() {
    final id = widget.desktopFileId;
    if (id == null || id.isEmpty) {
      _future = null;
      return;
    }
    if (widget.controller.hasResolvedIcon(id)) {
      _future = null;
      return;
    }
    _future = widget.controller.loadIcon(id);
  }

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final id = widget.desktopFileId;

    Widget buildFallback() => Container(
      width: widget.size,
      height: widget.size,
      decoration: BoxDecoration(
        color: scheme.secondaryContainer,
        borderRadius: BorderRadius.circular(widget.size * 0.25),
      ),
      child: Icon(
        widget.fallbackIcon,
        size: widget.size * 0.52,
        color: scheme.onSecondaryContainer,
      ),
    );

    Widget buildBytes(Uint8List bytes) => ClipRRect(
      borderRadius: BorderRadius.circular(widget.size * 0.22),
      child: Image.memory(
        bytes,
        width: widget.size,
        height: widget.size,
        fit: BoxFit.contain,
        gaplessPlayback: true,
        errorBuilder: (_, _, _) => buildFallback(),
      ),
    );

    if (id == null || id.isEmpty) return buildFallback();

    final cached = widget.controller.cachedIconSync(id);
    if (cached != null) return buildBytes(cached);
    if (widget.controller.hasResolvedIcon(id)) return buildFallback();

    return FutureBuilder<Uint8List?>(
      future: _future,
      builder: (context, snapshot) {
        final bytes = snapshot.data;
        if (bytes != null && bytes.isNotEmpty) {
          return buildBytes(bytes);
        }
        return buildFallback();
      },
    );
  }
}

/// Compact tonal pill used for sources, classifications, and statuses.
class InfoPill extends StatelessWidget {
  const InfoPill({
    super.key,
    required this.label,
    this.icon,
    this.backgroundColor,
    this.foregroundColor,
  });

  final String label;
  final IconData? icon;
  final Color? backgroundColor;
  final Color? foregroundColor;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final bg = backgroundColor ?? scheme.surfaceContainerHighest;
    final fg = foregroundColor ?? scheme.onSurfaceVariant;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
      decoration: BoxDecoration(
        color: bg,
        borderRadius: BorderRadius.circular(6),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          if (icon != null) ...[
            Icon(icon, size: 13, color: fg),
            const SizedBox(width: 4),
          ],
          Text(
            label,
            style: Theme.of(context).textTheme.labelSmall?.copyWith(
              color: fg,
              fontWeight: FontWeight.w600,
            ),
          ),
        ],
      ),
    );
  }
}

/// Consistent section card with header icon, title, and padded body.
class SectionCard extends StatelessWidget {
  const SectionCard({
    super.key,
    required this.title,
    this.icon,
    this.trailing,
    required this.child,
  });

  final String title;
  final IconData? icon;
  final Widget? trailing;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    return Card(
      margin: EdgeInsets.zero,
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                if (icon != null) ...[
                  Icon(icon, size: 18, color: scheme.primary),
                  const SizedBox(width: 8),
                ],
                Expanded(
                  child: Text(
                    title,
                    style: theme.textTheme.titleSmall?.copyWith(
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                ),
                ?trailing,
              ],
            ),
            const SizedBox(height: 12),
            child,
          ],
        ),
      ),
    );
  }
}

/// Clean key-value metadata row with selectable values.
class KeyValueRow extends StatelessWidget {
  const KeyValueRow({
    super.key,
    required this.label,
    required this.value,
    this.monospace = false,
  });

  final String label;
  final String value;
  final bool monospace;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SizedBox(
            width: 148,
            child: Text(
              label,
              style: theme.textTheme.bodySmall?.copyWith(
                color: scheme.onSurfaceVariant,
                fontWeight: FontWeight.w500,
              ),
            ),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: SelectableText(
              value,
              style: theme.textTheme.bodyMedium?.copyWith(
                fontFamily: monospace ? 'monospace' : null,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

/// Standardized empty, error, or loading state message.
class StateMessageView extends StatelessWidget {
  const StateMessageView({
    super.key,
    required this.icon,
    required this.title,
    this.subtitle,
    this.action,
    this.isError = false,
  });

  final IconData icon;
  final String title;
  final String? subtitle;
  final Widget? action;
  final bool isError;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 420),
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Container(
                padding: const EdgeInsets.all(14),
                decoration: BoxDecoration(
                  color: isError
                      ? scheme.errorContainer
                      : scheme.surfaceContainerHighest,
                  shape: BoxShape.circle,
                ),
                child: Icon(
                  icon,
                  size: 32,
                  color: isError
                      ? scheme.onErrorContainer
                      : scheme.onSurfaceVariant,
                ),
              ),
              const SizedBox(height: 16),
              Text(
                title,
                textAlign: TextAlign.center,
                style: theme.textTheme.titleMedium?.copyWith(
                  fontWeight: FontWeight.w600,
                ),
              ),
              if (subtitle != null) ...[
                const SizedBox(height: 8),
                Text(
                  subtitle!,
                  textAlign: TextAlign.center,
                  style: theme.textTheme.bodyMedium?.copyWith(
                    color: scheme.onSurfaceVariant,
                  ),
                ),
              ],
              if (action != null) ...[const SizedBox(height: 16), action!],
            ],
          ),
        ),
      ),
    );
  }
}

/// Compact warning banner displayed when discovery completed with partial
/// provider errors so the user can inspect provider diagnostics directly.
class PartialFailureBanner extends StatelessWidget {
  const PartialFailureBanner({
    super.key,
    required this.diagnostics,
    required this.onInspect,
  });

  final List<LinuxDiscoveryProviderDiagnostic> diagnostics;
  final VoidCallback onInspect;

  @override
  Widget build(BuildContext context) {
    final issues = diagnostics
        .where(
          (d) =>
              d.status == LinuxDiscoveryProviderStatus.failed ||
              d.status == LinuxDiscoveryProviderStatus.partiallyFailed,
        )
        .toList();
    if (issues.isEmpty) return const SizedBox.shrink();

    final scheme = Theme.of(context).colorScheme;
    final names = issues.map((d) => d.providerId).join(', ');
    return Container(
      margin: const EdgeInsets.fromLTRB(16, 12, 16, 0),
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
      decoration: BoxDecoration(
        color: scheme.tertiaryContainer,
        borderRadius: BorderRadius.circular(10),
      ),
      child: Row(
        children: [
          Icon(
            Icons.warning_amber_rounded,
            size: 20,
            color: scheme.onTertiaryContainer,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              'Partial discovery results: ${issues.length} provider(s) reported issues ($names).',
              style: Theme.of(context).textTheme.bodySmall?.copyWith(
                color: scheme.onTertiaryContainer,
                fontWeight: FontWeight.w500,
              ),
            ),
          ),
          TextButton(
            onPressed: onInspect,
            child: const Text('View diagnostics'),
          ),
        ],
      ),
    );
  }
}
