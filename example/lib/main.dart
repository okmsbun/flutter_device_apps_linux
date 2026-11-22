import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';
import 'package:material_ui/material_ui.dart';

void main() => runApp(const LinuxDeviceAppsExample());

class LinuxDeviceAppsExample extends StatelessWidget {
  const LinuxDeviceAppsExample({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
    title: 'Flutter Device Apps Linux',
    theme: ThemeData(colorScheme: ColorScheme.fromSeed(seedColor: Colors.teal)),
    home: const _AppBrowser(),
  );
}

class _AppBrowser extends StatefulWidget {
  const _AppBrowser();

  @override
  State<_AppBrowser> createState() => _AppBrowserState();
}

class _AppBrowserState extends State<_AppBrowser> {
  final _search = TextEditingController();
  late var _apps = FlutterDeviceAppsLinux.listApps();

  void _load(String query) {
    setState(() {
      _apps = query.trim().isEmpty
          ? FlutterDeviceAppsLinux.listApps()
          : FlutterDeviceAppsLinux.searchApps(query);
    });
  }

  @override
  void dispose() {
    _search.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(
      title: const Text('Flutter Device Apps Linux'),
      actions: [
        IconButton(
          tooltip: 'Find handlers',
          onPressed: () => Navigator.of(context).push(
            MaterialPageRoute<void>(builder: (_) => const _HandlerLookup()),
          ),
          icon: const Icon(Icons.find_in_page_outlined),
        ),
        IconButton(
          tooltip: 'Refresh apps',
          onPressed: () => _load(_search.text),
          icon: const Icon(Icons.refresh),
        ),
      ],
    ),
    body: Column(
      children: [
        Padding(
          padding: const EdgeInsets.all(16),
          child: TextField(
            controller: _search,
            decoration: InputDecoration(
              labelText: 'Search apps',
              border: const OutlineInputBorder(),
              suffixIcon: IconButton(
                tooltip: 'Search',
                onPressed: () => _load(_search.text),
                icon: const Icon(Icons.search),
              ),
            ),
            onSubmitted: _load,
          ),
        ),
        Expanded(
          child: FutureBuilder(
            future: _apps,
            builder: (context, snapshot) {
              if (snapshot.connectionState != ConnectionState.done) {
                return const Center(child: CircularProgressIndicator());
              }
              if (snapshot.hasError) {
                return Center(
                  child: Text('Unable to load apps\n${snapshot.error}'),
                );
              }
              final apps = snapshot.data!;
              if (apps.isEmpty) {
                return const Center(child: Text('No desktop apps found.'));
              }
              return ListView.builder(
                itemCount: apps.length,
                itemBuilder: (context, index) => _AppTile(app: apps[index]),
              );
            },
          ),
        ),
      ],
    ),
  );
}

class _AppTile extends StatelessWidget {
  const _AppTile({required this.app, this.isDefault = false});

  final LinuxAppInfo app;
  final bool isDefault;

  @override
  Widget build(BuildContext context) => ListTile(
    leading: const Icon(Icons.desktop_windows_outlined),
    title: Text(app.appName),
    subtitle: Text(app.desktopFileId),
    trailing: isDefault
        ? const Text('Default')
        : const Icon(Icons.chevron_right),
    onTap: () => Navigator.of(
      context,
    ).push(MaterialPageRoute<void>(builder: (_) => _AppDetails(entry: app))),
  );
}

class _AppDetails extends StatefulWidget {
  const _AppDetails({required this.entry});

  final LinuxAppInfo entry;

  @override
  State<_AppDetails> createState() => _AppDetailsState();
}

class _AppDetailsState extends State<_AppDetails> {
  String get _id => widget.entry.desktopFileId;
  late final _details = (
    FlutterDeviceAppsLinux.getApp(_id),
    FlutterDeviceAppsLinux.getAppIcon(_id),
    FlutterDeviceAppsLinux.getAppMetadata(_id),
    FlutterDeviceAppsLinux.getInstallSourceInfo(_id),
    FlutterDeviceAppsLinux.detectAppTechnologies(_id),
  ).wait;
  bool _opening = false;

  Future<void> _open() async {
    setState(() => _opening = true);
    String message;
    try {
      message = await FlutterDeviceAppsLinux.openApp(_id)
          ? 'Launch requested.'
          : 'The desktop entry is unavailable.';
    } catch (error) {
      message = 'Unable to open app\n$error';
    }
    if (!mounted) return;
    setState(() => _opening = false);
    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text(message)));
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: Text(widget.entry.appName)),
    body: FutureBuilder(
      future: _details,
      builder: (context, snapshot) {
        if (snapshot.connectionState != ConnectionState.done) {
          return const Center(child: CircularProgressIndicator());
        }
        if (snapshot.hasError) {
          return Center(
            child: Text('Unable to inspect app\n${snapshot.error}'),
          );
        }

        final (app, icon, metadata, source, tech) = snapshot.data!;
        if (app == null) {
          return const Center(child: Text('The desktop entry is unavailable.'));
        }
        final rows = <String, String?>{
          'Desktop ID': app.desktopFileId,
          'Description': metadata?.description ?? app.description,
          'Desktop file': app.desktopFilePath,
          'Categories': _joined(app.categories),
          'MIME types': _joined(app.mimeTypes),
          'Accepts files': app.supportsFiles ? 'Yes' : 'No',
          'Accepts URIs': app.supportsUris ? 'Yes' : 'No',
          'Requires terminal': app.requiresTerminal ? 'Yes' : 'No',
          'Homepage': metadata?.homepage?.toString(),
          'Developer': metadata?.developer,
          'License': metadata?.license,
          'Install source': source?.source.name,
          'Package ID': source?.packageId,
          'Version': source?.versionName,
          'Frameworks': _technologyNames(tech?.frameworks),
          'UI toolkits': _technologyNames(tech?.uiToolkits),
          'Libraries': _technologyNames(tech?.libraries),
        };
        return ListView(
          padding: const EdgeInsets.all(16),
          children: [
            Row(
              children: [
                if (icon != null) ...[
                  Image.memory(icon, width: 64, height: 64),
                  const SizedBox(width: 16),
                ],
                FilledButton.icon(
                  onPressed: _opening ? null : _open,
                  icon: const Icon(Icons.open_in_new),
                  label: const Text('Open app'),
                ),
              ],
            ),
            for (final field in rows.entries)
              if (field.value case final value?)
                ListTile(
                  title: Text(field.key),
                  subtitle: SelectableText(value),
                ),
          ],
        );
      },
    ),
  );

  static String? _joined(Iterable<String> values) =>
      values.isEmpty ? null : values.join(', ');

  static String? _technologyNames(List<LinuxAppTechnology>? values) =>
      _joined(values?.map((value) => value.name) ?? const []);
}

class _HandlerLookup extends StatefulWidget {
  const _HandlerLookup();

  @override
  State<_HandlerLookup> createState() => _HandlerLookupState();
}

class _HandlerLookupState extends State<_HandlerLookup> {
  final _input = TextEditingController();
  Future<(LinuxAppInfo?, List<LinuxAppInfo>)>? _result;

  void _lookup() {
    final value = _input.text.trim();
    if (value.isEmpty) return;
    final isFile = value.startsWith('/');
    setState(() {
      _result = isFile
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

  @override
  void dispose() {
    _input.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('Find handlers')),
    body: Column(
      children: [
        Padding(
          padding: const EdgeInsets.all(16),
          child: TextField(
            controller: _input,
            onSubmitted: (_) => _lookup(),
            decoration: InputDecoration(
              labelText: 'Absolute file path or MIME type',
              hintText: 'application/pdf',
              border: const OutlineInputBorder(),
              suffixIcon: IconButton(
                tooltip: 'Look up handlers',
                onPressed: _lookup,
                icon: const Icon(Icons.search),
              ),
            ),
          ),
        ),
        if (_result != null)
          Expanded(
            child: FutureBuilder(
              future: _result,
              builder: (context, snapshot) {
                if (snapshot.connectionState != ConnectionState.done) {
                  return const Center(child: CircularProgressIndicator());
                }
                if (snapshot.hasError) {
                  return Center(
                    child: Text(
                      'Unable to look up handlers\n${snapshot.error}',
                    ),
                  );
                }
                final (preferred, handlers) = snapshot.data!;
                if (preferred == null && handlers.isEmpty) {
                  return const Center(child: Text('No registered handlers.'));
                }
                return ListView(
                  children: [
                    if (preferred != null)
                      _AppTile(app: preferred, isDefault: true),
                    for (final app in handlers)
                      if (app.desktopFileId != preferred?.desktopFileId)
                        _AppTile(app: app),
                  ],
                );
              },
            ),
          ),
      ],
    ),
  );
}
