import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
import 'package:material_ui/material_ui.dart';

import '../explorer_controller.dart';
import '../widgets/shared_widgets.dart';

/// File path and MIME type handler lookup preserving `getDefaultAppForFile`,
/// `listAppsForFile`, `getDefaultAppForMimeType`, `listAppsForMimeType`, app
/// launching, and ELF technology detection.
class HandlersView extends StatefulWidget {
  const HandlersView({super.key, required this.controller});

  final DiscoveryExplorerController controller;

  @override
  State<HandlersView> createState() => _HandlersViewState();
}

class _HandlersViewState extends State<HandlersView> {
  final _input = TextEditingController();
  Future<(LinuxAppInfo?, List<LinuxAppInfo>)>? _lookupFuture;
  String? _activeQuery;
  bool _launching = false;

  static const _quickTypes = [
    'text/plain',
    'application/pdf',
    'image/png',
    'text/html',
  ];

  void _lookup([String? preset]) {
    if (preset != null) {
      _input.text = preset;
    }
    final value = _input.text.trim();
    if (value.isEmpty) return;
    final isFile = value.startsWith('/');
    setState(() {
      _activeQuery = value;
      _lookupFuture = isFile
          ? (
              FlutterDeviceAppsLinux.getDefaultAppForFile(value),
              FlutterDeviceAppsLinux.listAppsForFile(value),
            ).wait
          : (
              FlutterDeviceAppsLinux.getDefaultAppForMimeType(value),
              FlutterDeviceAppsLinux.listAppsForMimeType(value),
            ).wait;
    });
  }

  Future<void> _openApp(String desktopFileId) async {
    setState(() => _launching = true);
    String message;
    try {
      message = await FlutterDeviceAppsLinux.openApp(desktopFileId)
          ? 'Launch requested for $desktopFileId.'
          : 'The desktop entry is unavailable.';
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
  void dispose() {
    _input.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return Padding(
      padding: const EdgeInsets.all(20),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SectionCard(
            title: 'Find file or MIME handlers',
            icon: Icons.find_in_page_outlined,
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Expanded(
                      child: TextField(
                        key: const ValueKey('handlers_input_field'),
                        controller: _input,
                        onSubmitted: (_) => _lookup(),
                        decoration: InputDecoration(
                          labelText: 'Absolute file path or MIME type',
                          hintText: '/home/user/report.pdf or application/pdf',
                          prefixIcon: const Icon(Icons.search),
                          suffixIcon: IconButton(
                            tooltip: 'Look up handlers',
                            onPressed: _lookup,
                            icon: const Icon(Icons.arrow_forward_rounded),
                          ),
                        ),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 10),
                Wrap(
                  spacing: 8,
                  runSpacing: 6,
                  children: [
                    for (final mime in _quickTypes)
                      ActionChip(
                        label: Text(mime),
                        onPressed: () => _lookup(mime),
                      ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),
          Expanded(
            child: _lookupFuture == null
                ? const StateMessageView(
                    icon: Icons.file_open_outlined,
                    title: 'Inspect registered handlers',
                    subtitle:
                        'Enter an absolute local file path (starting with /) or a MIME type such as text/plain.',
                  )
                : FutureBuilder<(LinuxAppInfo?, List<LinuxAppInfo>)>(
                    future: _lookupFuture,
                    builder: (context, snapshot) {
                      if (snapshot.connectionState != ConnectionState.done) {
                        return const Center(child: CircularProgressIndicator());
                      }
                      if (snapshot.hasError) {
                        return StateMessageView(
                          icon: Icons.error_outline,
                          title: 'Handler lookup failed',
                          subtitle: '${snapshot.error}',
                          isError: true,
                        );
                      }
                      final (preferred, handlers) = snapshot.data!;
                      final alternatives = [
                        for (final app in handlers)
                          if (app.desktopFileId != preferred?.desktopFileId)
                            app,
                      ];
                      if (preferred == null && alternatives.isEmpty) {
                        return StateMessageView(
                          icon: Icons.link_off_rounded,
                          title: 'No registered handlers',
                          subtitle:
                              'No Linux desktop entry is registered for "$_activeQuery".',
                        );
                      }
                      return ListView(
                        children: [
                          if (preferred != null) ...[
                            _HandlerCard(
                              controller: widget.controller,
                              app: preferred,
                              isDefault: true,
                              launching: _launching,
                              onLaunch: () => _openApp(preferred.desktopFileId),
                            ),
                            const SizedBox(height: 10),
                          ],
                          if (alternatives.isNotEmpty) ...[
                            Padding(
                              padding: const EdgeInsets.only(bottom: 8, top: 4),
                              child: Text(
                                'Alternative handlers (${alternatives.length})',
                                style: Theme.of(context).textTheme.labelLarge
                                    ?.copyWith(color: scheme.onSurfaceVariant),
                              ),
                            ),
                            for (final app in alternatives) ...[
                              _HandlerCard(
                                controller: widget.controller,
                                app: app,
                                isDefault: false,
                                launching: _launching,
                                onLaunch: () => _openApp(app.desktopFileId),
                              ),
                              const SizedBox(height: 8),
                            ],
                          ],
                        ],
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }
}

class _HandlerCard extends StatelessWidget {
  const _HandlerCard({
    required this.controller,
    required this.app,
    required this.isDefault,
    required this.launching,
    required this.onLaunch,
  });

  final DiscoveryExplorerController controller;
  final LinuxAppInfo app;
  final bool isDefault;
  final bool launching;
  final VoidCallback onLaunch;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return Card(
      margin: EdgeInsets.zero,
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Row(
          children: [
            AppIconAvatar(
              controller: controller,
              desktopFileId: app.desktopFileId,
              fallbackIcon: Icons.desktop_windows_outlined,
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Flexible(
                        child: Text(
                          app.appName,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: Theme.of(context).textTheme.titleSmall
                              ?.copyWith(fontWeight: FontWeight.w600),
                        ),
                      ),
                      if (isDefault) ...[
                        const SizedBox(width: 8),
                        InfoPill(
                          label: 'Default',
                          backgroundColor: scheme.primaryContainer,
                          foregroundColor: scheme.onPrimaryContainer,
                        ),
                      ],
                    ],
                  ),
                  const SizedBox(height: 2),
                  Text(
                    app.desktopFileId,
                    style: Theme.of(context).textTheme.bodySmall?.copyWith(
                      color: scheme.onSurfaceVariant,
                      fontFamily: 'monospace',
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(width: 8),
            OutlinedButton.icon(
              onPressed: launching ? null : onLaunch,
              icon: const Icon(Icons.open_in_new, size: 16),
              label: const Text('Open'),
            ),
          ],
        ),
      ),
    );
  }
}
