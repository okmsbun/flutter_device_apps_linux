import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
import 'package:material_ui/material_ui.dart';

import '../explorer_controller.dart';
import '../widgets/shared_widgets.dart';

/// Overview dashboard displaying real statistics from the current
/// `LinuxDiscoveryResult` without fabricated numbers.
class DashboardView extends StatelessWidget {
  const DashboardView({super.key, required this.controller});

  final DiscoveryExplorerController controller;

  @override
  Widget build(BuildContext context) {
    final result = controller.result!;
    final apps = result.applications;
    final packages = result.packages;
    final diagnostics = result.providerDiagnostics;

    final sourceCounts = <LinuxInstallationSource, int>{};
    final classificationCounts = <LinuxAppClassification, int>{};
    final visibilityCounts = <LinuxDesktopMenuVisibility, int>{};
    final scopeCounts = <LinuxInstallationScope, int>{};

    for (final app in apps) {
      sourceCounts.update(
        app.primaryProvenance.installationSource,
        (c) => c + 1,
        ifAbsent: () => 1,
      );
      classificationCounts.update(
        app.classification,
        (c) => c + 1,
        ifAbsent: () => 1,
      );
      visibilityCounts.update(
        app.desktopMenuVisibility,
        (c) => c + 1,
        ifAbsent: () => 1,
      );
      scopeCounts.update(
        app.primaryProvenance.installationScope,
        (c) => c + 1,
        ifAbsent: () => 1,
      );
    }

    final totalIssues =
        controller.totalMetadataConflicts + controller.totalAmbiguities;

    return SingleChildScrollView(
      padding: const EdgeInsets.all(20),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          LayoutBuilder(
            builder: (context, constraints) {
              final columns = constraints.maxWidth >= 960
                  ? 4
                  : constraints.maxWidth >= 560
                  ? 2
                  : 1;
              final spacing = 12.0;
              final cardWidth =
                  (constraints.maxWidth - spacing * (columns - 1)) / columns;
              return Wrap(
                spacing: spacing,
                runSpacing: spacing,
                children: [
                  SizedBox(
                    width: cardWidth,
                    child: _StatCard(
                      icon: Icons.apps_rounded,
                      label: 'Applications',
                      value: '${apps.length}',
                      caption:
                          '${apps.where((a) => a.desktopEntries.isNotEmpty).length} with desktop entries',
                      onTap: () =>
                          controller.setSection(ExplorerSection.applications),
                    ),
                  ),
                  SizedBox(
                    width: cardWidth,
                    child: _StatCard(
                      icon: Icons.inventory_2_outlined,
                      label: 'Installed packages',
                      value: controller.includePackages
                          ? '${packages.length}'
                          : 'Off',
                      caption: controller.includePackages
                          ? 'Separate package inventory'
                          : 'Enable in Packages or Diagnostics',
                      onTap: () =>
                          controller.setSection(ExplorerSection.packages),
                    ),
                  ),
                  SizedBox(
                    width: cardWidth,
                    child: _StatCard(
                      icon: Icons.monitor_heart_outlined,
                      label: 'Active providers',
                      value:
                          '${controller.succeededProviderCount}/${diagnostics.length}',
                      caption: controller.hasPartialFailure
                          ? 'Some providers reported issues'
                          : 'All available providers healthy',
                      highlightWarning: controller.hasPartialFailure,
                      onTap: () =>
                          controller.setSection(ExplorerSection.diagnostics),
                    ),
                  ),
                  SizedBox(
                    width: cardWidth,
                    child: _StatCard(
                      icon: Icons.rule_folder_outlined,
                      label: 'Conflicts & ambiguities',
                      value: '$totalIssues',
                      caption:
                          '${controller.totalMetadataConflicts} conflicts · ${controller.totalAmbiguities} ambiguities',
                      highlightWarning: totalIssues > 0,
                      onTap: () =>
                          controller.setSection(ExplorerSection.diagnostics),
                    ),
                  ),
                ],
              );
            },
          ),
          const SizedBox(height: 16),
          LayoutBuilder(
            builder: (context, constraints) {
              final twoColumn = constraints.maxWidth >= 820;
              final sourceCard = SectionCard(
                title: 'Applications by installation source',
                icon: Icons.source_outlined,
                child: apps.isEmpty
                    ? const Text('No applications discovered.')
                    : Column(
                        children: [
                          for (final entry in _sortedEntries(sourceCounts))
                            _DistributionRow(
                              icon: iconForSource(entry.key),
                              label: formatSource(entry.key),
                              count: entry.value,
                              total: apps.length,
                              onTap: () => controller.openApplicationsFiltered(
                                source: entry.key,
                              ),
                            ),
                        ],
                      ),
              );

              final classificationCard = SectionCard(
                title: 'Applications by classification',
                icon: Icons.category_outlined,
                child: apps.isEmpty
                    ? const Text('No applications discovered.')
                    : Column(
                        children: [
                          for (final entry in _sortedEntries(
                            classificationCounts,
                          ))
                            _DistributionRow(
                              icon: iconForClassification(entry.key),
                              label: formatClassification(entry.key),
                              count: entry.value,
                              total: apps.length,
                              onTap: () => controller.openApplicationsFiltered(
                                classification: entry.key,
                              ),
                            ),
                        ],
                      ),
              );

              if (!twoColumn) {
                return Column(
                  children: [
                    sourceCard,
                    const SizedBox(height: 16),
                    classificationCard,
                  ],
                );
              }
              return Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Expanded(child: sourceCard),
                  const SizedBox(width: 16),
                  Expanded(child: classificationCard),
                ],
              );
            },
          ),
          const SizedBox(height: 16),
          LayoutBuilder(
            builder: (context, constraints) {
              final twoColumn = constraints.maxWidth >= 820;
              final scopeVisibilityCard = SectionCard(
                title: 'Menu visibility & installation scope',
                icon: Icons.visibility_outlined,
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      'Desktop menu visibility',
                      style: Theme.of(context).textTheme.labelMedium?.copyWith(
                        color: Theme.of(context).colorScheme.onSurfaceVariant,
                      ),
                    ),
                    const SizedBox(height: 8),
                    Wrap(
                      spacing: 8,
                      runSpacing: 8,
                      children: [
                        for (final v in LinuxDesktopMenuVisibility.values)
                          InfoPill(
                            label:
                                '${formatVisibility(v)}: ${visibilityCounts[v] ?? 0}',
                          ),
                      ],
                    ),
                    const SizedBox(height: 14),
                    Text(
                      'Installation scope',
                      style: Theme.of(context).textTheme.labelMedium?.copyWith(
                        color: Theme.of(context).colorScheme.onSurfaceVariant,
                      ),
                    ),
                    const SizedBox(height: 8),
                    Wrap(
                      spacing: 8,
                      runSpacing: 8,
                      children: [
                        for (final s in LinuxInstallationScope.values)
                          InfoPill(
                            label: '${formatScope(s)}: ${scopeCounts[s] ?? 0}',
                          ),
                      ],
                    ),
                  ],
                ),
              );

              final providersCard = SectionCard(
                title: 'Provider status',
                icon: Icons.hub_outlined,
                trailing: TextButton(
                  onPressed: () =>
                      controller.setSection(ExplorerSection.diagnostics),
                  child: const Text('Inspect all'),
                ),
                child: diagnostics.isEmpty
                    ? const Text('No provider diagnostics reported.')
                    : Wrap(
                        spacing: 8,
                        runSpacing: 8,
                        children: [
                          for (final diag in diagnostics)
                            InfoPill(
                              label:
                                  '${diag.providerId} · ${formatProviderStatus(diag.status)} (${diag.discoveredCount})',
                              backgroundColor: statusBackground(
                                Theme.of(context).colorScheme,
                                diag.status,
                              ),
                              foregroundColor: statusForeground(
                                Theme.of(context).colorScheme,
                                diag.status,
                              ),
                            ),
                        ],
                      ),
              );

              if (!twoColumn) {
                return Column(
                  children: [
                    scopeVisibilityCard,
                    const SizedBox(height: 16),
                    providersCard,
                  ],
                );
              }
              return Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Expanded(child: scopeVisibilityCard),
                  const SizedBox(width: 16),
                  Expanded(child: providersCard),
                ],
              );
            },
          ),
        ],
      ),
    );
  }

  static List<MapEntry<K, int>> _sortedEntries<K>(Map<K, int> map) {
    final entries = map.entries.toList()
      ..sort((a, b) => b.value.compareTo(a.value));
    return entries;
  }
}

class _StatCard extends StatelessWidget {
  const _StatCard({
    required this.icon,
    required this.label,
    required this.value,
    required this.caption,
    this.highlightWarning = false,
    this.onTap,
  });

  final IconData icon;
  final String label;
  final String value;
  final String caption;
  final bool highlightWarning;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    return Card(
      margin: EdgeInsets.zero,
      clipBehavior: Clip.antiAlias,
      child: InkWell(
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  Container(
                    padding: const EdgeInsets.all(8),
                    decoration: BoxDecoration(
                      color: highlightWarning
                          ? scheme.tertiaryContainer
                          : scheme.primaryContainer,
                      borderRadius: BorderRadius.circular(8),
                    ),
                    child: Icon(
                      icon,
                      size: 18,
                      color: highlightWarning
                          ? scheme.onTertiaryContainer
                          : scheme.onPrimaryContainer,
                    ),
                  ),
                  const Spacer(),
                  if (onTap != null)
                    Icon(
                      Icons.arrow_forward_rounded,
                      size: 16,
                      color: scheme.onSurfaceVariant,
                    ),
                ],
              ),
              const SizedBox(height: 14),
              Text(
                value,
                style: theme.textTheme.headlineMedium?.copyWith(
                  fontWeight: FontWeight.w700,
                ),
              ),
              const SizedBox(height: 2),
              Text(
                label,
                style: theme.textTheme.titleSmall?.copyWith(
                  fontWeight: FontWeight.w600,
                ),
              ),
              const SizedBox(height: 4),
              Text(
                caption,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: theme.textTheme.bodySmall?.copyWith(
                  color: scheme.onSurfaceVariant,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _DistributionRow extends StatelessWidget {
  const _DistributionRow({
    required this.icon,
    required this.label,
    required this.count,
    required this.total,
    required this.onTap,
  });

  final IconData icon;
  final String label;
  final int count;
  final int total;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    final ratio = total == 0 ? 0.0 : count / total;
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(8),
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 6, horizontal: 4),
        child: Column(
          children: [
            Row(
              children: [
                Icon(icon, size: 16, color: scheme.onSurfaceVariant),
                const SizedBox(width: 8),
                Expanded(child: Text(label, style: theme.textTheme.bodyMedium)),
                Text(
                  '$count',
                  style: theme.textTheme.labelLarge?.copyWith(
                    fontWeight: FontWeight.w600,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 6),
            ClipRRect(
              borderRadius: BorderRadius.circular(4),
              child: LinearProgressIndicator(
                value: ratio,
                minHeight: 5,
                backgroundColor: scheme.surfaceContainerHighest,
              ),
            ),
          ],
        ),
      ),
    );
  }
}
