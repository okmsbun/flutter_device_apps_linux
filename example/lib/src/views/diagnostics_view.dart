import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
import 'package:material_ui/material_ui.dart';

import '../explorer_controller.dart';
import '../widgets/shared_widgets.dart';

/// Displays per-provider diagnostics, discovery options, metadata conflicts,
/// and correlation ambiguities from the current `LinuxDiscoveryResult`.
class DiagnosticsView extends StatelessWidget {
  const DiagnosticsView({super.key, required this.controller});

  final DiscoveryExplorerController controller;

  @override
  Widget build(BuildContext context) {
    final result = controller.result!;
    final diagnostics = result.providerDiagnostics;
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;

    final appConflicts = <(LinuxDiscoveredApp, LinuxMetadataConflict)>[
      for (final app in result.applications)
        for (final conflict in app.metadataConflicts) (app, conflict),
    ];
    final appAmbiguities = <(LinuxDiscoveredApp, LinuxResolutionAmbiguity)>[
      for (final app in result.applications)
        for (final amb in app.ambiguities) (app, amb),
    ];

    return SingleChildScrollView(
      padding: const EdgeInsets.all(20),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SectionCard(
            title: 'Discovery pass options',
            icon: Icons.settings_outlined,
            trailing: FilledButton.tonalIcon(
              onPressed: controller.isLoading ? null : controller.refresh,
              icon: const Icon(Icons.refresh, size: 18),
              label: const Text('Run discovery'),
            ),
            child: Column(
              children: [
                SwitchListTile(
                  contentPadding: EdgeInsets.zero,
                  title: const Text('Include installed packages'),
                  subtitle: const Text(
                    'Populates LinuxDiscoveryResult.packages separately from confirmed applications.',
                  ),
                  value: controller.includePackages,
                  onChanged: controller.isLoading
                      ? null
                      : controller.setIncludePackages,
                ),
                SwitchListTile(
                  contentPadding: EdgeInsets.zero,
                  title: const Text('Include local AppStream metadata'),
                  subtitle: const Text(
                    'Enriches applications with authoritative classification, summaries, and catalog releases.',
                  ),
                  value: controller.includeAppStreamMetadata,
                  onChanged: controller.isLoading
                      ? null
                      : controller.setIncludeAppStreamMetadata,
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),

          SectionCard(
            title: 'Provider diagnostics (${diagnostics.length})',
            icon: Icons.monitor_heart_outlined,
            child: diagnostics.isEmpty
                ? const Text('No provider diagnostics reported.')
                : Column(
                    children: [
                      for (final diag in diagnostics)
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
                                      child: Text(
                                        diag.providerId,
                                        style: theme.textTheme.titleSmall
                                            ?.copyWith(
                                              fontFamily: 'monospace',
                                              fontWeight: FontWeight.w600,
                                            ),
                                      ),
                                    ),
                                    InfoPill(
                                      label: formatProviderStatus(diag.status),
                                      backgroundColor: statusBackground(
                                        scheme,
                                        diag.status,
                                      ),
                                      foregroundColor: statusForeground(
                                        scheme,
                                        diag.status,
                                      ),
                                    ),
                                    const SizedBox(width: 8),
                                    InfoPill(
                                      label: '${diag.discoveredCount} items',
                                    ),
                                  ],
                                ),
                                if (diag.errorMessage case final message?) ...[
                                  const SizedBox(height: 6),
                                  SelectableText(
                                    message,
                                    style: theme.textTheme.bodySmall?.copyWith(
                                      color: scheme.error,
                                    ),
                                  ),
                                ],
                              ],
                            ),
                          ),
                        ),
                    ],
                  ),
          ),
          const SizedBox(height: 16),

          SectionCard(
            title: 'Metadata conflicts (${appConflicts.length})',
            icon: Icons.compare_arrows_rounded,
            child: appConflicts.isEmpty
                ? Text(
                    'No field-level metadata disagreements detected across providers.',
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: scheme.onSurfaceVariant,
                    ),
                  )
                : Column(
                    children: [
                      for (final (app, conflict) in appConflicts)
                        ListTile(
                          contentPadding: EdgeInsets.zero,
                          title: Text(
                            '${DiscoveryExplorerController.displayNameForApp(app)} · ${conflict.fieldName}',
                          ),
                          subtitle: Text(
                            'Chose "${conflict.chosenValue}" (${conflict.chosenProviderId}) over "${conflict.conflictingValue}" (${conflict.conflictingProviderId})',
                          ),
                          trailing: const Icon(Icons.chevron_right),
                          onTap: () =>
                              controller.openAppInBrowser(app.resolvedId),
                        ),
                    ],
                  ),
          ),
          const SizedBox(height: 16),

          SectionCard(
            title:
                'Correlation ambiguities (${result.ambiguities.length + appAmbiguities.length})',
            icon: Icons.help_outline_rounded,
            child: (result.ambiguities.isEmpty && appAmbiguities.isEmpty)
                ? Text(
                    'No unresolved correlation ambiguities in this discovery pass.',
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: scheme.onSurfaceVariant,
                    ),
                  )
                : Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      for (final amb in result.ambiguities)
                        ListTile(
                          contentPadding: EdgeInsets.zero,
                          leading: const Icon(Icons.public_outlined),
                          title: Text(amb.reason),
                          subtitle: SelectableText(amb.candidateIds.join(', ')),
                        ),
                      for (final (app, amb) in appAmbiguities)
                        ListTile(
                          contentPadding: EdgeInsets.zero,
                          leading: const Icon(Icons.apps_outlined),
                          title: Text(
                            '${DiscoveryExplorerController.displayNameForApp(app)} · ${amb.reason}',
                          ),
                          subtitle: SelectableText(amb.candidateIds.join(', ')),
                          trailing: const Icon(Icons.chevron_right),
                          onTap: () =>
                              controller.openAppInBrowser(app.resolvedId),
                        ),
                    ],
                  ),
          ),
        ],
      ),
    );
  }
}
