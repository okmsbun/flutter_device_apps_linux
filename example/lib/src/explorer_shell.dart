import 'package:material_ui/material_ui.dart';

import 'explorer_controller.dart';
import 'views/applications_view.dart';
import 'views/dashboard_view.dart';
import 'views/diagnostics_view.dart';
import 'views/handlers_view.dart';
import 'views/packages_view.dart';
import 'widgets/shared_widgets.dart';

/// Responsive Material 3 application shell featuring a navigation sidebar on
/// desktop widths, bottom navigation on compact widths, theme switcher, and
/// unified loading/error/partial-failure states.
class LinuxExplorerShell extends StatefulWidget {
  const LinuxExplorerShell({
    super.key,
    required this.controller,
    required this.themeMode,
    required this.onThemeModeChanged,
  });

  final DiscoveryExplorerController controller;
  final ThemeMode themeMode;
  final ValueChanged<ThemeMode> onThemeModeChanged;

  @override
  State<LinuxExplorerShell> createState() => _LinuxExplorerShellState();
}

class _LinuxExplorerShellState extends State<LinuxExplorerShell> {
  @override
  void initState() {
    super.initState();
    widget.controller.ensureLoaded();
  }

  static const _destinations = [
    (
      section: ExplorerSection.dashboard,
      label: 'Dashboard',
      icon: Icons.dashboard_outlined,
      selectedIcon: Icons.dashboard,
    ),
    (
      section: ExplorerSection.applications,
      label: 'Applications',
      icon: Icons.apps_outlined,
      selectedIcon: Icons.apps,
    ),
    (
      section: ExplorerSection.packages,
      label: 'Packages',
      icon: Icons.inventory_2_outlined,
      selectedIcon: Icons.inventory_2,
    ),
    (
      section: ExplorerSection.handlers,
      label: 'Handlers',
      icon: Icons.find_in_page_outlined,
      selectedIcon: Icons.find_in_page,
    ),
    (
      section: ExplorerSection.diagnostics,
      label: 'Diagnostics',
      icon: Icons.monitor_heart_outlined,
      selectedIcon: Icons.monitor_heart,
    ),
  ];

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: widget.controller,
      builder: (context, _) {
        final controller = widget.controller;
        final selectedIndex = _destinations.indexWhere(
          (d) => d.section == controller.section,
        );
        final activeTitle = _destinations[selectedIndex].label;

        return LayoutBuilder(
          builder: (context, constraints) {
            final width = constraints.maxWidth;
            final showRail = width >= 720;
            final extendedRail = width >= 1120;
            final isMasterDetailWide = width >= 960;

            final bodyContent = _buildBody(
              context,
              controller,
              isMasterDetailWide: isMasterDetailWide,
            );

            return Scaffold(
              appBar: AppBar(
                title: Row(
                  children: [
                    Text(activeTitle),
                    if (controller.isLoading && controller.hasLoaded) ...[
                      const SizedBox(width: 12),
                      const SizedBox(
                        width: 16,
                        height: 16,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      ),
                    ],
                  ],
                ),
                actions: [
                  PopupMenuButton<ThemeMode>(
                    tooltip: 'Theme mode',
                    initialValue: widget.themeMode,
                    onSelected: widget.onThemeModeChanged,
                    icon: Icon(switch (widget.themeMode) {
                      ThemeMode.system => Icons.brightness_auto_outlined,
                      ThemeMode.light => Icons.light_mode_outlined,
                      ThemeMode.dark => Icons.dark_mode_outlined,
                    }),
                    itemBuilder: (_) => const [
                      PopupMenuItem(
                        value: ThemeMode.system,
                        child: Text('System theme'),
                      ),
                      PopupMenuItem(
                        value: ThemeMode.light,
                        child: Text('Light theme'),
                      ),
                      PopupMenuItem(
                        value: ThemeMode.dark,
                        child: Text('Dark theme'),
                      ),
                    ],
                  ),
                  IconButton(
                    tooltip: 'Refresh discovery',
                    onPressed: controller.isLoading ? null : controller.refresh,
                    icon: const Icon(Icons.refresh),
                  ),
                  const SizedBox(width: 8),
                ],
              ),
              body: Row(
                children: [
                  if (showRail) ...[
                    NavigationRail(
                      extended: extendedRail,
                      selectedIndex: selectedIndex,
                      onDestinationSelected: (index) =>
                          controller.setSection(_destinations[index].section),
                      leading: extendedRail
                          ? Padding(
                              padding: const EdgeInsets.fromLTRB(12, 8, 12, 16),
                              child: Row(
                                mainAxisSize: MainAxisSize.min,
                                children: [
                                  Icon(
                                    Icons.desktop_windows_rounded,
                                    color: Theme.of(
                                      context,
                                    ).colorScheme.primary,
                                  ),
                                  const SizedBox(width: 10),
                                  Text(
                                    'Linux Explorer',
                                    style: Theme.of(context)
                                        .textTheme
                                        .titleSmall
                                        ?.copyWith(fontWeight: FontWeight.w700),
                                  ),
                                ],
                              ),
                            )
                          : null,
                      destinations: [
                        for (final dest in _destinations)
                          NavigationRailDestination(
                            icon: Icon(dest.icon),
                            selectedIcon: Icon(dest.selectedIcon),
                            label: Text(dest.label),
                          ),
                      ],
                    ),
                    const VerticalDivider(width: 1),
                  ],
                  Expanded(child: bodyContent),
                ],
              ),
              bottomNavigationBar: showRail
                  ? null
                  : NavigationBar(
                      selectedIndex: selectedIndex,
                      onDestinationSelected: (index) =>
                          controller.setSection(_destinations[index].section),
                      destinations: [
                        for (final dest in _destinations)
                          NavigationDestination(
                            icon: Icon(dest.icon),
                            selectedIcon: Icon(dest.selectedIcon),
                            label: dest.label,
                          ),
                      ],
                    ),
            );
          },
        );
      },
    );
  }

  Widget _buildBody(
    BuildContext context,
    DiscoveryExplorerController controller, {
    required bool isMasterDetailWide,
  }) {
    if (controller.section == ExplorerSection.handlers) {
      return HandlersView(controller: controller);
    }

    if (controller.isLoading && !controller.hasLoaded) {
      return const StateMessageView(
        icon: Icons.radar_rounded,
        title: 'Discovering Linux applications',
        subtitle:
            'Querying available local providers (GIO, Flatpak, Snap, DPKG, RPM, Pacman, Nix, and AppStream)...',
        action: CircularProgressIndicator(),
      );
    }

    if (controller.error != null && controller.result == null) {
      return StateMessageView(
        icon: Icons.error_outline_rounded,
        title: 'Unable to complete discovery',
        subtitle: '${controller.error}',
        isError: true,
        action: FilledButton.icon(
          onPressed: controller.refresh,
          icon: const Icon(Icons.refresh),
          label: const Text('Retry'),
        ),
      );
    }

    if (controller.result == null) {
      return StateMessageView(
        icon: Icons.apps_outlined,
        title: 'Ready to inspect local apps',
        action: FilledButton(
          onPressed: controller.refresh,
          child: const Text('Start discovery'),
        ),
      );
    }

    final sectionView = switch (controller.section) {
      ExplorerSection.dashboard => DashboardView(controller: controller),
      ExplorerSection.applications => ApplicationsView(
        controller: controller,
        isWideLayout: isMasterDetailWide,
      ),
      ExplorerSection.packages => PackagesView(
        controller: controller,
        isWideLayout: isMasterDetailWide,
      ),
      ExplorerSection.handlers => HandlersView(controller: controller),
      ExplorerSection.diagnostics => DiagnosticsView(controller: controller),
    };

    return Column(
      children: [
        if (controller.hasPartialFailure &&
            controller.section != ExplorerSection.diagnostics)
          PartialFailureBanner(
            diagnostics: controller.providerDiagnostics,
            onInspect: () => controller.setSection(ExplorerSection.diagnostics),
          ),
        Expanded(child: sectionView),
      ],
    );
  }
}
