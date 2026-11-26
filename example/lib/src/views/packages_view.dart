import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
import 'package:material_ui/material_ui.dart';

import '../explorer_controller.dart';
import '../widgets/shared_widgets.dart';

/// Master-detail browser for raw [LinuxInstalledPackage] inventory records
/// kept strictly separate from confirmed applications.
class PackagesView extends StatefulWidget {
  const PackagesView({
    super.key,
    required this.controller,
    required this.isWideLayout,
  });

  final DiscoveryExplorerController controller;
  final bool isWideLayout;

  @override
  State<PackagesView> createState() => _PackagesViewState();
}

class _PackagesViewState extends State<PackagesView> {
  late final TextEditingController _searchController;
  bool _showFilters = false;

  @override
  void initState() {
    super.initState();
    _searchController = TextEditingController(
      text: widget.controller.packageQuery,
    );
    _showFilters = widget.controller.hasActivePackageFilters;
  }

  @override
  void didUpdateWidget(PackagesView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (_searchController.text != widget.controller.packageQuery) {
      _searchController.text = widget.controller.packageQuery;
    }
  }

  @override
  void dispose() {
    _searchController.dispose();
    super.dispose();
  }

  void _openDetails(LinuxInstalledPackage pkg) {
    final key = DiscoveryExplorerController.packageUniqueKey(pkg);
    widget.controller.selectPackage(key);
    if (!widget.isWideLayout) {
      Navigator.of(context).push(
        MaterialPageRoute<void>(
          builder: (_) => Scaffold(
            appBar: AppBar(title: Text(pkg.packageName)),
            body: PackageDetailPane(pkg: pkg),
          ),
        ),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    final controller = widget.controller;
    if (!controller.includePackages) {
      return StateMessageView(
        icon: Icons.inventory_2_outlined,
        title: 'Package inventory is disabled',
        subtitle:
            'Installed system and profile packages are kept separate from applications and loaded only when includePackages is enabled.',
        action: FilledButton.icon(
          onPressed: () => controller.setIncludePackages(true),
          icon: const Icon(Icons.download_outlined),
          label: const Text('Load installed packages'),
        ),
      );
    }

    final totalCount = controller.result?.packages.length ?? 0;
    final filtered = controller.filteredPackages;
    final selected = controller.selectedPackage;
    final availableSources = controller.availablePackageSources;

    final masterPane = Column(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 14, 16, 8),
          child: Row(
            children: [
              Expanded(
                child: TextField(
                  key: const ValueKey('packages_search_field'),
                  controller: _searchController,
                  onChanged: controller.setPackageQuery,
                  decoration: InputDecoration(
                    hintText: 'Search packages, versions, licenses...',
                    prefixIcon: const Icon(Icons.search),
                    suffixIcon: controller.packageQuery.isNotEmpty
                        ? IconButton(
                            tooltip: 'Clear search',
                            onPressed: () {
                              _searchController.clear();
                              controller.setPackageQuery('');
                            },
                            icon: const Icon(Icons.close),
                          )
                        : null,
                  ),
                ),
              ),
              const SizedBox(width: 8),
              IconButton.filledTonal(
                tooltip: 'Toggle package filters',
                isSelected: _showFilters || controller.hasActivePackageFilters,
                onPressed: () => setState(() => _showFilters = !_showFilters),
                icon: Icon(
                  controller.hasActivePackageFilters
                      ? Icons.filter_alt
                      : Icons.filter_alt_outlined,
                ),
              ),
              const SizedBox(width: 4),
              PopupMenuButton<PackageSortOption>(
                tooltip: 'Sort packages',
                initialValue: controller.packageSort,
                onSelected: controller.setPackageSort,
                icon: const Icon(Icons.sort_rounded),
                itemBuilder: (_) => const [
                  PopupMenuItem(
                    value: PackageSortOption.nameAsc,
                    child: Text('Name (A–Z)'),
                  ),
                  PopupMenuItem(
                    value: PackageSortOption.nameDesc,
                    child: Text('Name (Z–A)'),
                  ),
                  PopupMenuItem(
                    value: PackageSortOption.source,
                    child: Text('Package manager'),
                  ),
                  PopupMenuItem(
                    value: PackageSortOption.version,
                    child: Text('Version'),
                  ),
                ],
              ),
            ],
          ),
        ),
        if (_showFilters)
          Container(
            width: double.infinity,
            margin: const EdgeInsets.fromLTRB(16, 0, 16, 8),
            padding: const EdgeInsets.all(12),
            decoration: BoxDecoration(
              color: Theme.of(context).colorScheme.surfaceContainerLow,
              borderRadius: BorderRadius.circular(12),
              border: Border.all(
                color: Theme.of(context).colorScheme.outlineVariant,
              ),
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  'Package source & scope',
                  style: Theme.of(context).textTheme.labelSmall?.copyWith(
                    color: Theme.of(context).colorScheme.onSurfaceVariant,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                const SizedBox(height: 6),
                Wrap(
                  spacing: 6,
                  runSpacing: 6,
                  children: [
                    for (final source
                        in (availableSources.isEmpty
                            ? [
                                LinuxInstallationSource.dpkg,
                                LinuxInstallationSource.rpm,
                                LinuxInstallationSource.pacman,
                                LinuxInstallationSource.nix,
                              ]
                            : availableSources))
                      FilterChip(
                        label: Text(formatSource(source)),
                        selected: controller.packageSourceFilters.contains(
                          source,
                        ),
                        onSelected: (_) =>
                            controller.togglePackageSource(source),
                      ),
                    for (final scope in LinuxInstallationScope.values)
                      FilterChip(
                        label: Text(formatScope(scope)),
                        selected: controller.packageScopeFilters.contains(
                          scope,
                        ),
                        onSelected: (_) => controller.togglePackageScope(scope),
                      ),
                  ],
                ),
              ],
            ),
          ),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
          child: Row(
            children: [
              Text(
                'Showing ${filtered.length} of $totalCount packages',
                style: Theme.of(context).textTheme.bodySmall?.copyWith(
                  color: Theme.of(context).colorScheme.onSurfaceVariant,
                ),
              ),
              const Spacer(),
              if (controller.hasActivePackageFilters)
                TextButton(
                  onPressed: () {
                    _searchController.clear();
                    controller.clearPackageFilters();
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
                  icon: Icons.inventory_2_outlined,
                  title: totalCount == 0
                      ? 'No installed packages found'
                      : 'No matching packages',
                  subtitle: totalCount == 0
                      ? 'No local package database (DPKG, RPM, Pacman, Nix) returned packages.'
                      : 'Try broadening your search or clearing active filters.',
                  action: controller.hasActivePackageFilters
                      ? FilledButton.tonal(
                          onPressed: () {
                            _searchController.clear();
                            controller.clearPackageFilters();
                          },
                          child: const Text('Clear filters'),
                        )
                      : null,
                )
              : ListView.builder(
                  itemCount: filtered.length,
                  itemBuilder: (context, index) {
                    final pkg = filtered[index];
                    final key = DiscoveryExplorerController.packageUniqueKey(
                      pkg,
                    );
                    final isSelected =
                        widget.isWideLayout &&
                        selected != null &&
                        DiscoveryExplorerController.packageUniqueKey(
                              selected,
                            ) ==
                            key;
                    return _PackageListTile(
                      pkg: pkg,
                      selected: isSelected,
                      onTap: () => _openDetails(pkg),
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
                  icon: Icons.inventory_2_outlined,
                  title: 'Select a package',
                  subtitle:
                      'Inspect package identities, versions, architecture, installation states, licenses, and Nix profile paths.',
                )
              : PackageDetailPane(
                  key: ValueKey(
                    DiscoveryExplorerController.packageUniqueKey(selected),
                  ),
                  pkg: selected,
                ),
        ),
      ],
    );
  }
}

class _PackageListTile extends StatelessWidget {
  const _PackageListTile({
    required this.pkg,
    required this.selected,
    required this.onTap,
  });

  final LinuxInstalledPackage pkg;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final subtitleParts = [?pkg.version, ?pkg.architecture];
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
              Container(
                width: 36,
                height: 36,
                decoration: BoxDecoration(
                  color: scheme.surfaceContainerHighest,
                  borderRadius: BorderRadius.circular(8),
                ),
                child: Icon(
                  iconForSource(pkg.provenance.installationSource),
                  size: 18,
                  color: scheme.onSurfaceVariant,
                ),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      pkg.packageName,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: Theme.of(context).textTheme.titleSmall?.copyWith(
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                    if (subtitleParts.isNotEmpty) ...[
                      const SizedBox(height: 2),
                      Text(
                        subtitleParts.join(' · '),
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: Theme.of(context).textTheme.bodySmall?.copyWith(
                          color: scheme.onSurfaceVariant,
                        ),
                      ),
                    ],
                    const SizedBox(height: 4),
                    Wrap(
                      spacing: 6,
                      runSpacing: 4,
                      children: [
                        InfoPill(
                          label: formatSource(
                            pkg.provenance.installationSource,
                          ),
                        ),
                        if (pkg.installReason !=
                            LinuxPackageInstallReason.unknown)
                          InfoPill(
                            label: formatPackageInstallReason(
                              pkg.installReason,
                            ),
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

/// Detailed inspector for a selected [LinuxInstalledPackage].
class PackageDetailPane extends StatelessWidget {
  const PackageDetailPane({super.key, required this.pkg});

  final LinuxInstalledPackage pkg;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    return SingleChildScrollView(
      padding: const EdgeInsets.all(20),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Card(
            margin: EdgeInsets.zero,
            child: Padding(
              padding: const EdgeInsets.all(20),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    pkg.packageName,
                    style: theme.textTheme.headlineSmall?.copyWith(
                      fontWeight: FontWeight.w700,
                    ),
                  ),
                  if (pkg.summary case final s?) ...[
                    const SizedBox(height: 4),
                    Text(
                      s,
                      style: theme.textTheme.bodyMedium?.copyWith(
                        color: scheme.onSurfaceVariant,
                      ),
                    ),
                  ],
                  const SizedBox(height: 12),
                  Wrap(
                    spacing: 8,
                    runSpacing: 6,
                    children: [
                      InfoPill(
                        label: formatSource(pkg.provenance.installationSource),
                        icon: iconForSource(pkg.provenance.installationSource),
                        backgroundColor: scheme.primaryContainer,
                        foregroundColor: scheme.onPrimaryContainer,
                      ),
                      InfoPill(
                        label: formatScope(pkg.provenance.installationScope),
                      ),
                      InfoPill(
                        label: formatPackageInstallState(pkg.installationState),
                      ),
                      if (pkg.installReason !=
                          LinuxPackageInstallReason.unknown)
                        InfoPill(
                          label: formatPackageInstallReason(pkg.installReason),
                        ),
                    ],
                  ),
                ],
              ),
            ),
          ),
          const SizedBox(height: 16),
          SectionCard(
            title: 'Package identity & version',
            icon: Icons.badge_outlined,
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                KeyValueRow(
                  label: 'Package identity',
                  value: pkg.packageIdentityId,
                  monospace: true,
                ),
                if (pkg.installationIdentityId case final id?)
                  KeyValueRow(
                    label: 'Installation ID',
                    value: id,
                    monospace: true,
                  ),
                KeyValueRow(
                  label: 'Provider',
                  value:
                      '${pkg.provenance.providerId} (${pkg.provenance.nativeId})',
                ),
                if (pkg.epoch case final v?)
                  KeyValueRow(label: 'Epoch', value: v),
                if (pkg.version case final v?)
                  KeyValueRow(label: 'Version', value: v),
                if (pkg.release case final v?)
                  KeyValueRow(label: 'Release', value: v),
                if (pkg.architecture case final v?)
                  KeyValueRow(label: 'Architecture', value: v),
                KeyValueRow(
                  label: 'Selection state',
                  value: formatPackageSelectionState(pkg.selectionState),
                ),
                KeyValueRow(
                  label: 'Classification',
                  value: formatClassification(pkg.classification),
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),
          SectionCard(
            title: 'Metadata',
            icon: Icons.description_outlined,
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                if (pkg.section case final v?)
                  KeyValueRow(label: 'Section', value: v),
                if (pkg.packageGroup case final v?)
                  KeyValueRow(label: 'Package group', value: v),
                if (pkg.groups.isNotEmpty)
                  KeyValueRow(label: 'Groups', value: pkg.groups.join(', ')),
                if (pkg.priority case final v?)
                  KeyValueRow(label: 'Priority', value: v),
                if (pkg.license case final v?)
                  KeyValueRow(label: 'License', value: v),
                if (pkg.licenses.isNotEmpty)
                  KeyValueRow(
                    label: 'Licenses',
                    value: pkg.licenses.join(', '),
                  ),
                if (pkg.homepage case final v?)
                  KeyValueRow(label: 'Homepage', value: v),
                if (pkg.maintainer case final v?)
                  KeyValueRow(label: 'Maintainer', value: v),
                if (pkg.packager case final v?)
                  KeyValueRow(label: 'Packager', value: v),
                if (pkg.vendor case final v?)
                  KeyValueRow(label: 'Vendor', value: v),
                if (pkg.description case final v?)
                  KeyValueRow(label: 'Description', value: v),
                if (pkg.profilePath case final v?)
                  KeyValueRow(label: 'Nix profile', value: v, monospace: true),
                if (pkg.attributePath case final v?)
                  KeyValueRow(
                    label: 'Attribute path',
                    value: v,
                    monospace: true,
                  ),
                if (pkg.sourceUrl case final v?)
                  KeyValueRow(label: 'Source URL', value: v),
                if (pkg.lockedUrl case final v?)
                  KeyValueRow(label: 'Locked URL', value: v),
                if (pkg.installPriority case final v?)
                  KeyValueRow(label: 'Install priority', value: '$v'),
                if (pkg.outputs.isNotEmpty)
                  KeyValueRow(label: 'Outputs', value: pkg.outputs.join(', ')),
                if (pkg.storePaths.isNotEmpty)
                  KeyValueRow(
                    label: 'Store paths',
                    value: pkg.storePaths.join('\n'),
                    monospace: true,
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
