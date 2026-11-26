import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
import 'package:material_ui/material_ui.dart';

import '../explorer_controller.dart';
import '../widgets/shared_widgets.dart';

/// Master-detail browser for normalized `LinuxDiscoveredApp` records with
/// instant local search, sorting, advanced filtering, app launch, and on-demand
/// desktop/ELF technology inspection.
class ApplicationsView extends StatefulWidget {
  const ApplicationsView({
    super.key,
    required this.controller,
    required this.isWideLayout,
  });

  final DiscoveryExplorerController controller;
  final bool isWideLayout;

  @override
  State<ApplicationsView> createState() => _ApplicationsViewState();
}

class _ApplicationsViewState extends State<ApplicationsView> {
  late final TextEditingController _searchController;
  bool _showFilters = false;

  @override
  void initState() {
    super.initState();
    _searchController = TextEditingController(text: widget.controller.appQuery);
    _showFilters = widget.controller.hasActiveAppFilters;
  }

  @override
  void didUpdateWidget(ApplicationsView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (_searchController.text != widget.controller.appQuery) {
      _searchController.text = widget.controller.appQuery;
    }
  }

  @override
  void dispose() {
    _searchController.dispose();
    super.dispose();
  }

  void _openDetails(LinuxDiscoveredApp app) {
    widget.controller.selectApp(app.resolvedId);
    if (!widget.isWideLayout) {
      Navigator.of(context).push(
        MaterialPageRoute<void>(
          builder: (_) => Scaffold(
            appBar: AppBar(
              title: Text(DiscoveryExplorerController.displayNameForApp(app)),
            ),
            body: ApplicationDetailPane(
              controller: widget.controller,
              app: app,
            ),
          ),
        ),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    final controller = widget.controller;
    final totalCount = controller.result?.applications.length ?? 0;
    final filtered = controller.filteredApps;
    final selected = controller.selectedApp;

    final masterPane = Column(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 14, 16, 8),
          child: Row(
            children: [
              Expanded(
                child: TextField(
                  key: const ValueKey('apps_search_field'),
                  controller: _searchController,
                  onChanged: controller.setAppQuery,
                  decoration: InputDecoration(
                    hintText: 'Search applications, IDs, executables...',
                    prefixIcon: const Icon(Icons.search),
                    suffixIcon: controller.appQuery.isNotEmpty
                        ? IconButton(
                            tooltip: 'Clear search',
                            onPressed: () {
                              _searchController.clear();
                              controller.setAppQuery('');
                            },
                            icon: const Icon(Icons.close),
                          )
                        : null,
                  ),
                ),
              ),
              const SizedBox(width: 8),
              IconButton.filledTonal(
                tooltip: 'Toggle filters',
                isSelected: _showFilters || controller.hasActiveAppFilters,
                onPressed: () => setState(() => _showFilters = !_showFilters),
                icon: Icon(
                  controller.hasActiveAppFilters
                      ? Icons.filter_alt
                      : Icons.filter_alt_outlined,
                ),
              ),
              const SizedBox(width: 4),
              PopupMenuButton<AppSortOption>(
                tooltip: 'Sort applications',
                initialValue: controller.appSort,
                onSelected: controller.setAppSort,
                icon: const Icon(Icons.sort_rounded),
                itemBuilder: (_) => const [
                  PopupMenuItem(
                    value: AppSortOption.nameAsc,
                    child: Text('Name (A–Z)'),
                  ),
                  PopupMenuItem(
                    value: AppSortOption.nameDesc,
                    child: Text('Name (Z–A)'),
                  ),
                  PopupMenuItem(
                    value: AppSortOption.source,
                    child: Text('Installation source'),
                  ),
                  PopupMenuItem(
                    value: AppSortOption.classification,
                    child: Text('Classification'),
                  ),
                  PopupMenuItem(
                    value: AppSortOption.desktopEntries,
                    child: Text('Desktop entries count'),
                  ),
                ],
              ),
            ],
          ),
        ),
        if (_showFilters) _AppFilterPanel(controller: controller),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
          child: Row(
            children: [
              Text(
                'Showing ${filtered.length} of $totalCount applications',
                style: Theme.of(context).textTheme.bodySmall?.copyWith(
                  color: Theme.of(context).colorScheme.onSurfaceVariant,
                ),
              ),
              const Spacer(),
              if (controller.hasActiveAppFilters)
                TextButton(
                  onPressed: () {
                    _searchController.clear();
                    controller.clearAppFilters();
                  },
                  child: const Text('Reset filters'),
                ),
            ],
          ),
        ),
        const Divider(height: 1),
        Expanded(
          child: filtered.isEmpty
              ? StateMessageView(
                  icon: Icons.search_off_rounded,
                  title: totalCount == 0
                      ? 'No applications discovered'
                      : 'No matching applications',
                  subtitle: totalCount == 0
                      ? 'Check Provider Diagnostics to see which local sources were available.'
                      : 'Try adjusting your search query or active filters.',
                  action: controller.hasActiveAppFilters
                      ? FilledButton.tonal(
                          onPressed: () {
                            _searchController.clear();
                            controller.clearAppFilters();
                          },
                          child: const Text('Clear filters'),
                        )
                      : null,
                )
              : ListView.builder(
                  itemCount: filtered.length,
                  itemBuilder: (context, index) {
                    final app = filtered[index];
                    final isSelected =
                        widget.isWideLayout &&
                        selected?.resolvedId == app.resolvedId;
                    return _ApplicationListTile(
                      controller: controller,
                      app: app,
                      selected: isSelected,
                      onTap: () => _openDetails(app),
                    );
                  },
                ),
        ),
      ],
    );

    if (!widget.isWideLayout) return masterPane;

    return Row(
      children: [
        SizedBox(width: 410, child: masterPane),
        const VerticalDivider(width: 1),
        Expanded(
          child: selected == null
              ? const StateMessageView(
                  icon: Icons.touch_app_outlined,
                  title: 'Select an application',
                  subtitle:
                      'Inspect installed metadata, capabilities, provenance, desktop entries, and linked ELF technologies.',
                )
              : ApplicationDetailPane(
                  key: ValueKey(selected.resolvedId),
                  controller: controller,
                  app: selected,
                ),
        ),
      ],
    );
  }
}

class _AppFilterPanel extends StatelessWidget {
  const _AppFilterPanel({required this.controller});

  final DiscoveryExplorerController controller;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    return Container(
      width: double.infinity,
      margin: const EdgeInsets.fromLTRB(16, 0, 16, 8),
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: scheme.surfaceContainerLow,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: scheme.outlineVariant),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          _FilterGroupLabel('Installation source'),
          Wrap(
            spacing: 6,
            runSpacing: 6,
            children: [
              for (final source in LinuxInstallationSource.values)
                FilterChip(
                  label: Text(formatSource(source)),
                  selected: controller.appSourceFilters.contains(source),
                  onSelected: (_) => controller.toggleAppSource(source),
                ),
            ],
          ),
          const SizedBox(height: 10),
          _FilterGroupLabel('Classification'),
          Wrap(
            spacing: 6,
            runSpacing: 6,
            children: [
              for (final kind in LinuxAppClassification.values)
                FilterChip(
                  label: Text(formatClassification(kind)),
                  selected: controller.appClassificationFilters.contains(kind),
                  onSelected: (_) => controller.toggleAppClassification(kind),
                ),
            ],
          ),
          const SizedBox(height: 10),
          _FilterGroupLabel('Scope & menu visibility'),
          Wrap(
            spacing: 6,
            runSpacing: 6,
            children: [
              for (final scope in LinuxInstallationScope.values)
                FilterChip(
                  label: Text(formatScope(scope)),
                  selected: controller.appScopeFilters.contains(scope),
                  onSelected: (_) => controller.toggleAppScope(scope),
                ),
              for (final vis in LinuxDesktopMenuVisibility.values)
                FilterChip(
                  label: Text(formatVisibility(vis)),
                  selected: controller.appVisibilityFilters.contains(vis),
                  onSelected: (_) => controller.toggleAppVisibility(vis),
                ),
              FilterChip(
                label: const Text('Has desktop entry'),
                selected: controller.onlyLaunchableDesktop,
                onSelected: controller.setOnlyLaunchableDesktop,
              ),
              FilterChip(
                label: const Text('Conflicts / ambiguities'),
                selected: controller.onlyWithIssues,
                onSelected: controller.setOnlyWithIssues,
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _FilterGroupLabel extends StatelessWidget {
  const _FilterGroupLabel(this.text);
  final String text;

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.only(bottom: 6),
    child: Text(
      text,
      style: Theme.of(context).textTheme.labelSmall?.copyWith(
        color: Theme.of(context).colorScheme.onSurfaceVariant,
        fontWeight: FontWeight.w600,
      ),
    ),
  );
}

class _ApplicationListTile extends StatelessWidget {
  const _ApplicationListTile({
    required this.controller,
    required this.app,
    required this.selected,
    required this.onTap,
  });

  final DiscoveryExplorerController controller;
  final LinuxDiscoveredApp app;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final name = DiscoveryExplorerController.displayNameForApp(app);
    final primaryDesktopId = app.desktopEntries.isNotEmpty
        ? app.desktopEntries.first.desktopFileId
        : null;
    final hasIssues =
        app.metadataConflicts.isNotEmpty || app.ambiguities.isNotEmpty;

    return Material(
      color: selected
          ? scheme.secondaryContainer.withValues(alpha: 0.55)
          : Colors.transparent,
      child: InkWell(
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
          child: Row(
            children: [
              AppIconAvatar(
                controller: controller,
                desktopFileId: primaryDesktopId,
                fallbackIcon: iconForClassification(app.classification),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Expanded(
                          child: Text(
                            name,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: Theme.of(context).textTheme.titleSmall
                                ?.copyWith(fontWeight: FontWeight.w600),
                          ),
                        ),
                        if (app.metadata.version case final version?) ...[
                          const SizedBox(width: 6),
                          Text(
                            version,
                            style: Theme.of(context).textTheme.labelSmall
                                ?.copyWith(color: scheme.onSurfaceVariant),
                          ),
                        ],
                      ],
                    ),
                    const SizedBox(height: 2),
                    Text(
                      primaryDesktopId ?? app.resolvedId,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: Theme.of(context).textTheme.bodySmall?.copyWith(
                        color: scheme.onSurfaceVariant,
                      ),
                    ),
                    const SizedBox(height: 6),
                    Wrap(
                      spacing: 6,
                      runSpacing: 4,
                      children: [
                        InfoPill(
                          label: formatSource(
                            app.primaryProvenance.installationSource,
                          ),
                          icon: iconForSource(
                            app.primaryProvenance.installationSource,
                          ),
                        ),
                        if (app.classification !=
                            LinuxAppClassification.unknown)
                          InfoPill(
                            label: formatClassification(app.classification),
                            backgroundColor: scheme.primaryContainer,
                            foregroundColor: scheme.onPrimaryContainer,
                          ),
                        if (hasIssues)
                          InfoPill(
                            label: 'Review',
                            icon: Icons.warning_amber_rounded,
                            backgroundColor: scheme.tertiaryContainer,
                            foregroundColor: scheme.onTertiaryContainer,
                          ),
                      ],
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

/// Detailed inspector for a selected [LinuxDiscoveredApp].
class ApplicationDetailPane extends StatefulWidget {
  const ApplicationDetailPane({
    super.key,
    required this.controller,
    required this.app,
  });

  final DiscoveryExplorerController controller;
  final LinuxDiscoveredApp app;

  @override
  State<ApplicationDetailPane> createState() => _ApplicationDetailPaneState();
}

class _ApplicationDetailPaneState extends State<ApplicationDetailPane> {
  Future<DesktopEntryInspection>? _inspectionFuture;
  bool _launching = false;

  String? get _primaryDesktopId => widget.app.desktopEntries.isNotEmpty
      ? widget.app.desktopEntries.first.desktopFileId
      : null;

  @override
  void initState() {
    super.initState();
    _loadInspection();
  }

  @override
  void didUpdateWidget(ApplicationDetailPane oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.app.resolvedId != widget.app.resolvedId) {
      _loadInspection();
    }
  }

  void _loadInspection() {
    final id = _primaryDesktopId;
    _inspectionFuture = id == null
        ? null
        : widget.controller.inspectDesktopEntry(id);
  }

  Future<void> _launchDesktopEntry(String desktopFileId) async {
    setState(() => _launching = true);
    String message;
    try {
      final ok = await FlutterDeviceAppsLinux.openApp(desktopFileId);
      message = ok
          ? 'Launch requested for $desktopFileId.'
          : 'The desktop entry $desktopFileId is unavailable.';
    } catch (error) {
      message = 'Unable to open app: $error';
    }
    if (!mounted) return;
    setState(() => _launching = false);
    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text(message)));
  }

  @override
  Widget build(BuildContext context) {
    final app = widget.app;
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    final name = DiscoveryExplorerController.displayNameForApp(app);
    final primaryDesktopId = _primaryDesktopId;

    return SingleChildScrollView(
      padding: const EdgeInsets.all(20),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Header card
          Card(
            margin: EdgeInsets.zero,
            child: Padding(
              padding: const EdgeInsets.all(20),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  AppIconAvatar(
                    controller: widget.controller,
                    desktopFileId: primaryDesktopId,
                    fallbackIcon: iconForClassification(app.classification),
                    size: 56,
                  ),
                  const SizedBox(width: 16),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          name,
                          style: theme.textTheme.headlineSmall?.copyWith(
                            fontWeight: FontWeight.w700,
                          ),
                        ),
                        if (app.metadata.summary case final summary?) ...[
                          const SizedBox(height: 4),
                          Text(
                            summary,
                            style: theme.textTheme.bodyMedium?.copyWith(
                              color: scheme.onSurfaceVariant,
                            ),
                          ),
                        ],
                        const SizedBox(height: 10),
                        Wrap(
                          spacing: 8,
                          runSpacing: 6,
                          children: [
                            InfoPill(
                              label: formatSource(
                                app.primaryProvenance.installationSource,
                              ),
                              icon: iconForSource(
                                app.primaryProvenance.installationSource,
                              ),
                              backgroundColor: scheme.primaryContainer,
                              foregroundColor: scheme.onPrimaryContainer,
                            ),
                            InfoPill(
                              label: formatClassification(app.classification),
                              icon: iconForClassification(app.classification),
                            ),
                            InfoPill(
                              label: formatScope(
                                app.primaryProvenance.installationScope,
                              ),
                            ),
                            InfoPill(
                              label: formatVisibility(
                                app.desktopMenuVisibility,
                              ),
                            ),
                          ],
                        ),
                      ],
                    ),
                  ),
                  if (primaryDesktopId != null) ...[
                    const SizedBox(width: 12),
                    FilledButton.icon(
                      onPressed: _launching
                          ? null
                          : () => _launchDesktopEntry(primaryDesktopId),
                      icon: const Icon(Icons.open_in_new),
                      label: const Text('Open app'),
                    ),
                  ],
                ],
              ),
            ),
          ),
          const SizedBox(height: 16),

          // Metadata & Installed Version
          SectionCard(
            title: 'Metadata & installed version',
            icon: Icons.info_outline,
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                KeyValueRow(
                  label: 'Resolved ID',
                  value: app.resolvedId,
                  monospace: true,
                ),
                if (app.metadata.version case final v?)
                  KeyValueRow(label: 'Installed version', value: v),
                if (app.metadata.revision case final v?)
                  KeyValueRow(label: 'Revision', value: v),
                if (app.metadata.channel case final v?)
                  KeyValueRow(label: 'Channel', value: v),
                if (app.metadata.branch case final v?)
                  KeyValueRow(label: 'Branch', value: v),
                if (app.metadata.architecture case final v?)
                  KeyValueRow(label: 'Architecture', value: v),
                if (app.metadata.executable case final v?)
                  KeyValueRow(label: 'Executable', value: v, monospace: true),
                if (app.metadata.license case final v?)
                  KeyValueRow(label: 'License', value: v),
                if (app.metadata.homepage case final v?)
                  KeyValueRow(label: 'Homepage', value: v),
                if (app.metadata.iconReference case final v?)
                  KeyValueRow(
                    label: 'Icon reference',
                    value: v,
                    monospace: true,
                  ),
                if (app.metadata.description case final v?)
                  KeyValueRow(label: 'Description', value: v),
              ],
            ),
          ),

          // Upstream Catalog Releases (kept distinct from installed version)
          if (app.catalogReleases.latestVersion != null ||
              app.catalogReleases.releases.isNotEmpty) ...[
            const SizedBox(height: 16),
            SectionCard(
              title: 'Upstream AppStream catalog releases',
              icon: Icons.new_releases_outlined,
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  if (app.catalogReleases.latestVersion case final v?)
                    KeyValueRow(label: 'Latest catalog version', value: v),
                  if (app.catalogReleases.latestDate case final d?)
                    KeyValueRow(label: 'Latest release date', value: d),
                  for (final rel in app.catalogReleases.releases.take(5))
                    Padding(
                      padding: const EdgeInsets.only(top: 6),
                      child: Wrap(
                        spacing: 8,
                        runSpacing: 4,
                        children: [
                          if (rel.version case final v?) InfoPill(label: v),
                          if (rel.date case final d?)
                            Text(d, style: theme.textTheme.bodySmall),
                          if (rel.urgency case final u?)
                            InfoPill(label: 'Urgency: $u'),
                        ],
                      ),
                    ),
                ],
              ),
            ),
          ],

          const SizedBox(height: 16),
          // Capabilities
          SectionCard(
            title: 'Capabilities',
            icon: Icons.tune_outlined,
            child: Wrap(
              spacing: 10,
              runSpacing: 8,
              children: [
                _CapabilityPill('Launchable', app.capabilities.launchable),
                _CapabilityPill(
                  'Accepts files',
                  app.capabilities.supportsFiles,
                ),
                _CapabilityPill('Accepts URIs', app.capabilities.supportsUris),
                _CapabilityPill(
                  'Requires terminal',
                  app.capabilities.requiresTerminal,
                ),
              ],
            ),
          ),

          const SizedBox(height: 16),
          // Desktop Entries & On-demand ELF Technology Detection
          SectionCard(
            title: 'Desktop entries (${app.desktopEntries.length})',
            icon: Icons.desktop_windows_outlined,
            child: app.desktopEntries.isEmpty
                ? Text(
                    'No .desktop entry is associated with this installation. Desktop entries are never fabricated.',
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: scheme.onSurfaceVariant,
                    ),
                  )
                : Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      for (final entry in app.desktopEntries)
                        Padding(
                          padding: const EdgeInsets.only(bottom: 10),
                          child: Container(
                            padding: const EdgeInsets.all(12),
                            decoration: BoxDecoration(
                              color: scheme.surfaceContainerLow,
                              borderRadius: BorderRadius.circular(10),
                            ),
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                Row(
                                  children: [
                                    Expanded(
                                      child: SelectableText(
                                        entry.desktopFileId,
                                        style: theme.textTheme.titleSmall
                                            ?.copyWith(
                                              fontFamily: 'monospace',
                                              fontWeight: FontWeight.w600,
                                            ),
                                      ),
                                    ),
                                    TextButton.icon(
                                      onPressed: _launching
                                          ? null
                                          : () => _launchDesktopEntry(
                                              entry.desktopFileId,
                                            ),
                                      icon: const Icon(
                                        Icons.play_arrow_rounded,
                                        size: 18,
                                      ),
                                      label: const Text('Launch'),
                                    ),
                                  ],
                                ),
                                if (entry.desktopFilePath case final path?)
                                  KeyValueRow(
                                    label: 'Path',
                                    value: path,
                                    monospace: true,
                                  ),
                                KeyValueRow(
                                  label: 'Menu visibility',
                                  value: formatVisibility(
                                    entry.desktopMenuVisibility,
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ),
                      if (_inspectionFuture != null)
                        FutureBuilder<DesktopEntryInspection>(
                          future: _inspectionFuture,
                          builder: (context, snapshot) {
                            if (snapshot.connectionState !=
                                ConnectionState.done) {
                              return const Padding(
                                padding: EdgeInsets.symmetric(vertical: 8),
                                child: LinearProgressIndicator(),
                              );
                            }
                            final inspection = snapshot.data;
                            if (inspection == null) {
                              return const SizedBox.shrink();
                            }
                            final info = inspection.appInfo;
                            final tech = inspection.technologies;
                            return Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                if (info != null && info.categories.isNotEmpty)
                                  KeyValueRow(
                                    label: 'Categories',
                                    value: info.categories.join(', '),
                                  ),
                                if (info != null && info.mimeTypes.isNotEmpty)
                                  KeyValueRow(
                                    label: 'MIME types',
                                    value: info.mimeTypes.join(', '),
                                  ),
                                const SizedBox(height: 8),
                                Text(
                                  'Detected ELF technologies',
                                  style: theme.textTheme.labelMedium?.copyWith(
                                    fontWeight: FontWeight.w600,
                                  ),
                                ),
                                const SizedBox(height: 6),
                                if (tech == null ||
                                    (tech.frameworks.isEmpty &&
                                        tech.uiToolkits.isEmpty &&
                                        tech.libraries.isEmpty))
                                  Text(
                                    'No direct ELF technologies detected (may use a script or sandboxed launcher).',
                                    style: theme.textTheme.bodySmall?.copyWith(
                                      color: scheme.onSurfaceVariant,
                                    ),
                                  )
                                else
                                  Wrap(
                                    spacing: 6,
                                    runSpacing: 6,
                                    children: [
                                      for (final fw in tech.frameworks)
                                        InfoPill(
                                          label: 'Framework: ${fw.name}',
                                          backgroundColor:
                                              scheme.primaryContainer,
                                          foregroundColor:
                                              scheme.onPrimaryContainer,
                                        ),
                                      for (final ui in tech.uiToolkits)
                                        InfoPill(
                                          label: 'UI: ${ui.name}',
                                          backgroundColor:
                                              scheme.secondaryContainer,
                                          foregroundColor:
                                              scheme.onSecondaryContainer,
                                        ),
                                      for (final lib in tech.libraries)
                                        InfoPill(label: 'Lib: ${lib.name}'),
                                    ],
                                  ),
                              ],
                            );
                          },
                        ),
                    ],
                  ),
          ),

          const SizedBox(height: 16),
          // Provenance & Linked Identities
          SectionCard(
            title: 'Provenance & correlated records',
            icon: Icons.account_tree_outlined,
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                KeyValueRow(
                  label: 'Primary provider',
                  value:
                      '${app.primaryProvenance.providerId} (${app.primaryProvenance.nativeId})',
                ),
                if (app.sourceProvenances.isNotEmpty)
                  KeyValueRow(
                    label: 'All sources',
                    value: app.sourceProvenances
                        .map(
                          (p) =>
                              '${p.providerId}:${p.nativeId} [${formatSource(p.installationSource)}/${formatScope(p.installationScope)}]',
                        )
                        .join('\n'),
                  ),
                if (app.originalRecordIds.isNotEmpty)
                  KeyValueRow(
                    label: 'Original record IDs',
                    value: app.originalRecordIds.join('\n'),
                    monospace: true,
                  ),
                if (app.linkedAppStreamComponentIds.isNotEmpty)
                  KeyValueRow(
                    label: 'AppStream IDs',
                    value: app.linkedAppStreamComponentIds.join(', '),
                    monospace: true,
                  ),
                if (app.linkedPackageIdentityIds.isNotEmpty) ...[
                  const SizedBox(height: 6),
                  Text(
                    'Linked packages',
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: scheme.onSurfaceVariant,
                      fontWeight: FontWeight.w500,
                    ),
                  ),
                  const SizedBox(height: 6),
                  Wrap(
                    spacing: 8,
                    runSpacing: 6,
                    children: [
                      for (final pkgId in app.linkedPackageIdentityIds)
                        ActionChip(
                          avatar: const Icon(
                            Icons.inventory_2_outlined,
                            size: 16,
                          ),
                          label: Text(pkgId),
                          onPressed: () =>
                              widget.controller.openPackageByIdentity(pkgId),
                        ),
                    ],
                  ),
                ],
              ],
            ),
          ),

          // Conflicts & Ambiguities
          if (app.metadataConflicts.isNotEmpty ||
              app.ambiguities.isNotEmpty) ...[
            const SizedBox(height: 16),
            SectionCard(
              title: 'Metadata conflicts & ambiguities',
              icon: Icons.warning_amber_rounded,
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  for (final conflict in app.metadataConflicts)
                    Padding(
                      padding: const EdgeInsets.only(bottom: 8),
                      child: Container(
                        width: double.infinity,
                        padding: const EdgeInsets.all(10),
                        decoration: BoxDecoration(
                          color: scheme.tertiaryContainer.withValues(
                            alpha: 0.45,
                          ),
                          borderRadius: BorderRadius.circular(8),
                        ),
                        child: Text(
                          'Field "${conflict.fieldName}": chose "${conflict.chosenValue}" (${conflict.chosenProviderId}) over "${conflict.conflictingValue}" (${conflict.conflictingProviderId}).',
                          style: theme.textTheme.bodySmall,
                        ),
                      ),
                    ),
                  for (final amb in app.ambiguities)
                    Padding(
                      padding: const EdgeInsets.only(bottom: 8),
                      child: Container(
                        width: double.infinity,
                        padding: const EdgeInsets.all(10),
                        decoration: BoxDecoration(
                          color: scheme.surfaceContainerHighest,
                          borderRadius: BorderRadius.circular(8),
                        ),
                        child: Text(
                          'Ambiguity (${amb.reason}): ${amb.candidateIds.join(', ')}',
                          style: theme.textTheme.bodySmall,
                        ),
                      ),
                    ),
                ],
              ),
            ),
          ],
        ],
      ),
    );
  }
}

class _CapabilityPill extends StatelessWidget {
  const _CapabilityPill(this.label, this.state);

  final String label;
  final LinuxCapabilityState state;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final (bg, fg, icon) = switch (state) {
      LinuxCapabilityState.supported => (
        scheme.primaryContainer,
        scheme.onPrimaryContainer,
        Icons.check_circle_outline,
      ),
      LinuxCapabilityState.unsupported => (
        scheme.surfaceContainerHighest,
        scheme.onSurfaceVariant,
        Icons.cancel_outlined,
      ),
      LinuxCapabilityState.unknown => (
        scheme.surfaceContainerHigh,
        scheme.onSurfaceVariant,
        Icons.help_outline,
      ),
    };
    return InfoPill(
      label: '$label: ${formatCapability(state)}',
      icon: icon,
      backgroundColor: bg,
      foregroundColor: fg,
    );
  }
}
