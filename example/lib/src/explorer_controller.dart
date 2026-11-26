import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_device_apps_linux/flutter_device_apps_linux.dart';

/// Top-level sections in the Linux Application Explorer.
enum ExplorerSection {
  dashboard,
  applications,
  packages,
  handlers,
  diagnostics,
}

/// Sort orders supported by the application browser.
enum AppSortOption { nameAsc, nameDesc, source, classification, desktopEntries }

/// Sort orders supported by the installed package browser.
enum PackageSortOption { nameAsc, nameDesc, source, version }

/// On-demand detail inspection results loaded for a desktop entry.
@immutable
class DesktopEntryInspection {
  const DesktopEntryInspection({this.appInfo, this.technologies});

  final LinuxAppInfo? appInfo;
  final LinuxAppTechnologies? technologies;
}

typedef DiscoveryLoader =
    Future<LinuxDiscoveryResult> Function({
      required bool includePackages,
      required bool includeAppStreamMetadata,
    });

/// Manages unified discovery state, local filtering/sorting, and bounded caches
/// so browsing, searching, and switching tabs never trigger redundant native
/// discovery requests.
class DiscoveryExplorerController extends ChangeNotifier {
  DiscoveryExplorerController({
    bool includePackages = true,
    bool includeAppStreamMetadata = true,
    DiscoveryLoader? loader,
  }) : _loader = loader ?? FlutterDeviceAppsLinux.discoverApps {
    _includePackages = includePackages;
    _includeAppStreamMetadata = includeAppStreamMetadata;
  }

  final DiscoveryLoader _loader;

  late bool _includePackages;
  late bool _includeAppStreamMetadata;
  bool _isLoading = false;
  bool _hasLoaded = false;
  Object? _error;
  LinuxDiscoveryResult? _result;
  DateTime? _lastRefreshedAt;
  int _generation = 0;

  // Navigation & selection
  ExplorerSection _section = ExplorerSection.dashboard;
  String? _selectedAppId;
  String? _selectedPackageId;

  // Application filters & sorting
  String _appQuery = '';
  final Set<LinuxInstallationSource> _appSourceFilters = {};
  final Set<LinuxAppClassification> _appClassificationFilters = {};
  final Set<LinuxInstallationScope> _appScopeFilters = {};
  final Set<LinuxDesktopMenuVisibility> _appVisibilityFilters = {};
  bool _onlyLaunchableDesktop = false;
  bool _onlyWithIssues = false;
  AppSortOption _appSort = AppSortOption.nameAsc;
  List<LinuxDiscoveredApp>? _cachedFilteredApps;

  // Package filters & sorting
  String _packageQuery = '';
  final Set<LinuxInstallationSource> _packageSourceFilters = {};
  final Set<LinuxInstallationScope> _packageScopeFilters = {};
  PackageSortOption _packageSort = PackageSortOption.nameAsc;
  List<LinuxInstalledPackage>? _cachedFilteredPackages;

  // Caches keyed by desktopFileId
  final Map<String, Future<Uint8List?>> _iconFutures = {};
  final Map<String, Uint8List?> _resolvedIcons = {};
  final Map<String, Future<DesktopEntryInspection>> _inspectionFutures = {};

  bool get includePackages => _includePackages;
  bool get includeAppStreamMetadata => _includeAppStreamMetadata;
  bool get isLoading => _isLoading;
  bool get hasLoaded => _hasLoaded;
  Object? get error => _error;
  LinuxDiscoveryResult? get result => _result;
  DateTime? get lastRefreshedAt => _lastRefreshedAt;

  ExplorerSection get section => _section;
  String? get selectedAppId => _selectedAppId;
  String? get selectedPackageId => _selectedPackageId;

  String get appQuery => _appQuery;
  Set<LinuxInstallationSource> get appSourceFilters =>
      Set.unmodifiable(_appSourceFilters);
  Set<LinuxAppClassification> get appClassificationFilters =>
      Set.unmodifiable(_appClassificationFilters);
  Set<LinuxInstallationScope> get appScopeFilters =>
      Set.unmodifiable(_appScopeFilters);
  Set<LinuxDesktopMenuVisibility> get appVisibilityFilters =>
      Set.unmodifiable(_appVisibilityFilters);
  bool get onlyLaunchableDesktop => _onlyLaunchableDesktop;
  bool get onlyWithIssues => _onlyWithIssues;
  AppSortOption get appSort => _appSort;

  bool get hasActiveAppFilters =>
      _appQuery.trim().isNotEmpty ||
      _appSourceFilters.isNotEmpty ||
      _appClassificationFilters.isNotEmpty ||
      _appScopeFilters.isNotEmpty ||
      _appVisibilityFilters.isNotEmpty ||
      _onlyLaunchableDesktop ||
      _onlyWithIssues;

  String get packageQuery => _packageQuery;
  Set<LinuxInstallationSource> get packageSourceFilters =>
      Set.unmodifiable(_packageSourceFilters);
  Set<LinuxInstallationScope> get packageScopeFilters =>
      Set.unmodifiable(_packageScopeFilters);
  PackageSortOption get packageSort => _packageSort;

  bool get hasActivePackageFilters =>
      _packageQuery.trim().isNotEmpty ||
      _packageSourceFilters.isNotEmpty ||
      _packageScopeFilters.isNotEmpty;

  /// Diagnostics helpers derived directly from the latest discovery pass.
  List<LinuxDiscoveryProviderDiagnostic> get providerDiagnostics =>
      _result?.providerDiagnostics ?? const [];

  bool get hasPartialFailure => providerDiagnostics.any(
    (d) =>
        d.status == LinuxDiscoveryProviderStatus.partiallyFailed ||
        d.status == LinuxDiscoveryProviderStatus.failed,
  );

  int get succeededProviderCount => providerDiagnostics
      .where((d) => d.status == LinuxDiscoveryProviderStatus.succeeded)
      .length;

  int get totalMetadataConflicts => (_result?.applications ?? const []).fold(
    0,
    (sum, app) => sum + app.metadataConflicts.length,
  );

  int get totalAmbiguities {
    final res = _result;
    if (res == null) return 0;
    final perApp = res.applications.fold(
      0,
      (sum, app) => sum + app.ambiguities.length,
    );
    return res.ambiguities.length + perApp;
  }

  /// All available sources/scopes/classifications present in the current pass.
  Set<LinuxInstallationSource> get availableAppSources => {
    for (final app in _result?.applications ?? const <LinuxDiscoveredApp>[])
      app.primaryProvenance.installationSource,
  };

  Set<LinuxInstallationSource> get availablePackageSources => {
    for (final pkg in _result?.packages ?? const <LinuxInstalledPackage>[])
      pkg.provenance.installationSource,
  };

  /// Loads discovery results once if not yet loaded.
  Future<void> ensureLoaded() {
    if (_hasLoaded || _isLoading) return Future<void>.value();
    return refresh();
  }

  /// Performs a fresh unified discovery pass via `discoverApps()`.
  Future<void> refresh() async {
    final requestGen = ++_generation;
    _isLoading = true;
    _error = null;
    notifyListeners();

    try {
      final inventory = await _loader(
        includePackages: _includePackages,
        includeAppStreamMetadata: _includeAppStreamMetadata,
      );
      if (requestGen != _generation) return;

      _result = inventory;
      _hasLoaded = true;
      _lastRefreshedAt = DateTime.now();
      _iconFutures.clear();
      _resolvedIcons.clear();
      _inspectionFutures.clear();
      _invalidateFilters();

      // Keep selection valid if the same item still exists.
      if (_selectedAppId != null &&
          !inventory.applications.any((a) => a.resolvedId == _selectedAppId)) {
        _selectedAppId = null;
      }
      if (_selectedPackageId != null &&
          !inventory.packages.any(
            (p) => packageUniqueKey(p) == _selectedPackageId,
          )) {
        _selectedPackageId = null;
      }
    } catch (err) {
      if (requestGen != _generation) return;
      _error = err;
    } finally {
      if (requestGen == _generation) {
        _isLoading = false;
        notifyListeners();
      }
    }
  }

  Future<void> setIncludePackages(bool value) async {
    if (_includePackages == value) return;
    _includePackages = value;
    notifyListeners();
    await refresh();
  }

  Future<void> setIncludeAppStreamMetadata(bool value) async {
    if (_includeAppStreamMetadata == value) return;
    _includeAppStreamMetadata = value;
    notifyListeners();
    await refresh();
  }

  void setSection(ExplorerSection value) {
    if (_section == value) return;
    _section = value;
    notifyListeners();
  }

  void selectApp(String? resolvedId) {
    if (_selectedAppId == resolvedId) return;
    _selectedAppId = resolvedId;
    notifyListeners();
  }

  void openAppInBrowser(String resolvedId) {
    _section = ExplorerSection.applications;
    _selectedAppId = resolvedId;
    notifyListeners();
  }

  void openApplicationsFiltered({
    LinuxInstallationSource? source,
    LinuxAppClassification? classification,
    bool onlyWithIssues = false,
  }) {
    _appQuery = '';
    _appSourceFilters.clear();
    _appClassificationFilters.clear();
    _appScopeFilters.clear();
    _appVisibilityFilters.clear();
    _onlyLaunchableDesktop = false;
    _onlyWithIssues = onlyWithIssues;
    if (source != null) _appSourceFilters.add(source);
    if (classification != null) _appClassificationFilters.add(classification);
    _invalidateFilters();
    _section = ExplorerSection.applications;
    notifyListeners();
  }

  void selectPackage(String? key) {
    if (_selectedPackageId == key) return;
    _selectedPackageId = key;
    notifyListeners();
  }

  void openPackageByIdentity(String packageIdentityId) {
    final pkgs = _result?.packages ?? const <LinuxInstalledPackage>[];
    final match = pkgs
        .where(
          (p) =>
              p.packageIdentityId == packageIdentityId ||
              p.installationIdentityId == packageIdentityId,
        )
        .firstOrNull;
    _section = ExplorerSection.packages;
    if (match != null) {
      _selectedPackageId = packageUniqueKey(match);
    } else {
      _packageQuery = packageIdentityId;
      _invalidateFilters();
    }
    notifyListeners();
  }

  // --- Local application filters & sorting (zero native requests) ---

  void setAppQuery(String query) {
    if (_appQuery == query) return;
    _appQuery = query;
    _invalidateFilters();
    notifyListeners();
  }

  void toggleAppSource(LinuxInstallationSource source) {
    if (!_appSourceFilters.remove(source)) {
      _appSourceFilters.add(source);
    }
    _invalidateFilters();
    notifyListeners();
  }

  void toggleAppClassification(LinuxAppClassification classification) {
    if (!_appClassificationFilters.remove(classification)) {
      _appClassificationFilters.add(classification);
    }
    _invalidateFilters();
    notifyListeners();
  }

  void toggleAppScope(LinuxInstallationScope scope) {
    if (!_appScopeFilters.remove(scope)) {
      _appScopeFilters.add(scope);
    }
    _invalidateFilters();
    notifyListeners();
  }

  void toggleAppVisibility(LinuxDesktopMenuVisibility visibility) {
    if (!_appVisibilityFilters.remove(visibility)) {
      _appVisibilityFilters.add(visibility);
    }
    _invalidateFilters();
    notifyListeners();
  }

  void setOnlyLaunchableDesktop(bool value) {
    if (_onlyLaunchableDesktop == value) return;
    _onlyLaunchableDesktop = value;
    _invalidateFilters();
    notifyListeners();
  }

  void setOnlyWithIssues(bool value) {
    if (_onlyWithIssues == value) return;
    _onlyWithIssues = value;
    _invalidateFilters();
    notifyListeners();
  }

  void setAppSort(AppSortOption sort) {
    if (_appSort == sort) return;
    _appSort = sort;
    _invalidateFilters();
    notifyListeners();
  }

  void clearAppFilters() {
    _appQuery = '';
    _appSourceFilters.clear();
    _appClassificationFilters.clear();
    _appScopeFilters.clear();
    _appVisibilityFilters.clear();
    _onlyLaunchableDesktop = false;
    _onlyWithIssues = false;
    _invalidateFilters();
    notifyListeners();
  }

  List<LinuxDiscoveredApp> get filteredApps {
    final cached = _cachedFilteredApps;
    if (cached != null) return cached;

    final apps = _result?.applications ?? const <LinuxDiscoveredApp>[];
    final q = _appQuery.trim().toLowerCase();

    final list = <LinuxDiscoveredApp>[];
    for (final app in apps) {
      if (_appSourceFilters.isNotEmpty &&
          !_appSourceFilters.contains(
            app.primaryProvenance.installationSource,
          )) {
        continue;
      }
      if (_appClassificationFilters.isNotEmpty &&
          !_appClassificationFilters.contains(app.classification)) {
        continue;
      }
      if (_appScopeFilters.isNotEmpty &&
          !_appScopeFilters.contains(app.primaryProvenance.installationScope)) {
        continue;
      }
      if (_appVisibilityFilters.isNotEmpty &&
          !_appVisibilityFilters.contains(app.desktopMenuVisibility)) {
        continue;
      }
      if (_onlyLaunchableDesktop && app.desktopEntries.isEmpty) {
        continue;
      }
      if (_onlyWithIssues &&
          app.metadataConflicts.isEmpty &&
          app.ambiguities.isEmpty) {
        continue;
      }
      if (q.isNotEmpty && !_matchesAppQuery(app, q)) {
        continue;
      }
      list.add(app);
    }

    list.sort(_compareApps);
    _cachedFilteredApps = List<LinuxDiscoveredApp>.unmodifiable(list);
    return _cachedFilteredApps!;
  }

  LinuxDiscoveredApp? get selectedApp {
    final id = _selectedAppId;
    if (id == null) return null;
    for (final app in _result?.applications ?? const <LinuxDiscoveredApp>[]) {
      if (app.resolvedId == id) return app;
    }
    return null;
  }

  // --- Local package filters & sorting ---

  void setPackageQuery(String query) {
    if (_packageQuery == query) return;
    _packageQuery = query;
    _invalidateFilters();
    notifyListeners();
  }

  void togglePackageSource(LinuxInstallationSource source) {
    if (!_packageSourceFilters.remove(source)) {
      _packageSourceFilters.add(source);
    }
    _invalidateFilters();
    notifyListeners();
  }

  void togglePackageScope(LinuxInstallationScope scope) {
    if (!_packageScopeFilters.remove(scope)) {
      _packageScopeFilters.add(scope);
    }
    _invalidateFilters();
    notifyListeners();
  }

  void setPackageSort(PackageSortOption sort) {
    if (_packageSort == sort) return;
    _packageSort = sort;
    _invalidateFilters();
    notifyListeners();
  }

  void clearPackageFilters() {
    _packageQuery = '';
    _packageSourceFilters.clear();
    _packageScopeFilters.clear();
    _invalidateFilters();
    notifyListeners();
  }

  List<LinuxInstalledPackage> get filteredPackages {
    final cached = _cachedFilteredPackages;
    if (cached != null) return cached;

    final pkgs = _result?.packages ?? const <LinuxInstalledPackage>[];
    final q = _packageQuery.trim().toLowerCase();

    final list = <LinuxInstalledPackage>[];
    for (final pkg in pkgs) {
      if (_packageSourceFilters.isNotEmpty &&
          !_packageSourceFilters.contains(pkg.provenance.installationSource)) {
        continue;
      }
      if (_packageScopeFilters.isNotEmpty &&
          !_packageScopeFilters.contains(pkg.provenance.installationScope)) {
        continue;
      }
      if (q.isNotEmpty && !_matchesPackageQuery(pkg, q)) {
        continue;
      }
      list.add(pkg);
    }

    list.sort(_comparePackages);
    _cachedFilteredPackages = List<LinuxInstalledPackage>.unmodifiable(list);
    return _cachedFilteredPackages!;
  }

  LinuxInstalledPackage? get selectedPackage {
    final key = _selectedPackageId;
    if (key == null) return null;
    for (final pkg in _result?.packages ?? const <LinuxInstalledPackage>[]) {
      if (packageUniqueKey(pkg) == key) return pkg;
    }
    return null;
  }

  // --- Lazy bounded caches for icons and desktop entry inspection ---

  Uint8List? cachedIconSync(String desktopFileId) =>
      _resolvedIcons[desktopFileId];

  bool hasResolvedIcon(String desktopFileId) =>
      _resolvedIcons.containsKey(desktopFileId);

  Future<Uint8List?> loadIcon(String desktopFileId) {
    return _iconFutures.putIfAbsent(desktopFileId, () async {
      try {
        final bytes = await FlutterDeviceAppsLinux.getAppIcon(desktopFileId);
        _resolvedIcons[desktopFileId] = bytes;
        return bytes;
      } catch (_) {
        _resolvedIcons[desktopFileId] = null;
        return null;
      }
    });
  }

  Future<DesktopEntryInspection> inspectDesktopEntry(String desktopFileId) {
    return _inspectionFutures.putIfAbsent(desktopFileId, () async {
      LinuxAppInfo? info;
      LinuxAppTechnologies? tech;
      try {
        info = await FlutterDeviceAppsLinux.getApp(desktopFileId);
      } catch (_) {
        info = null;
      }
      try {
        tech = await FlutterDeviceAppsLinux.detectAppTechnologies(
          desktopFileId,
        );
      } catch (_) {
        tech = null;
      }
      return DesktopEntryInspection(appInfo: info, technologies: tech);
    });
  }

  static String displayNameForApp(LinuxDiscoveredApp app) {
    final metaName = app.metadata.name?.trim();
    if (metaName != null && metaName.isNotEmpty) return metaName;
    for (final entry in app.desktopEntries) {
      final entryName = entry.displayName?.trim();
      if (entryName != null && entryName.isNotEmpty) return entryName;
    }
    if (app.primaryProvenance.nativeId.isNotEmpty) {
      return app.primaryProvenance.nativeId;
    }
    return app.resolvedId;
  }

  static String packageUniqueKey(LinuxInstalledPackage pkg) =>
      pkg.installationIdentityId ?? pkg.packageIdentityId;

  void _invalidateFilters() {
    _cachedFilteredApps = null;
    _cachedFilteredPackages = null;
  }

  bool _matchesAppQuery(LinuxDiscoveredApp app, String q) {
    bool contains(String? value) =>
        value != null && value.toLowerCase().contains(q);

    if (contains(displayNameForApp(app)) ||
        contains(app.resolvedId) ||
        contains(app.primaryProvenance.providerId) ||
        contains(app.primaryProvenance.nativeId) ||
        contains(app.metadata.version) ||
        contains(app.metadata.summary) ||
        contains(app.metadata.description) ||
        contains(app.metadata.executable) ||
        contains(app.metadata.license) ||
        contains(app.metadata.homepage)) {
      return true;
    }
    for (final d in app.desktopEntries) {
      if (contains(d.desktopFileId) ||
          contains(d.displayName) ||
          contains(d.desktopFilePath)) {
        return true;
      }
    }
    for (final id in app.linkedPackageIdentityIds) {
      if (id.toLowerCase().contains(q)) return true;
    }
    for (final id in app.linkedAppStreamComponentIds) {
      if (id.toLowerCase().contains(q)) return true;
    }
    return false;
  }

  int _compareApps(LinuxDiscoveredApp a, LinuxDiscoveredApp b) {
    final nameA = displayNameForApp(a).toLowerCase();
    final nameB = displayNameForApp(b).toLowerCase();
    return switch (_appSort) {
      AppSortOption.nameAsc =>
        nameA.compareTo(nameB) != 0
            ? nameA.compareTo(nameB)
            : a.resolvedId.compareTo(b.resolvedId),
      AppSortOption.nameDesc =>
        nameB.compareTo(nameA) != 0
            ? nameB.compareTo(nameA)
            : b.resolvedId.compareTo(a.resolvedId),
      AppSortOption.source =>
        a.primaryProvenance.installationSource.name.compareTo(
                  b.primaryProvenance.installationSource.name,
                ) !=
                0
            ? a.primaryProvenance.installationSource.name.compareTo(
                b.primaryProvenance.installationSource.name,
              )
            : nameA.compareTo(nameB),
      AppSortOption.classification =>
        a.classification.name.compareTo(b.classification.name) != 0
            ? a.classification.name.compareTo(b.classification.name)
            : nameA.compareTo(nameB),
      AppSortOption.desktopEntries =>
        b.desktopEntries.length.compareTo(a.desktopEntries.length) != 0
            ? b.desktopEntries.length.compareTo(a.desktopEntries.length)
            : nameA.compareTo(nameB),
    };
  }

  bool _matchesPackageQuery(LinuxInstalledPackage pkg, String q) {
    bool contains(String? value) =>
        value != null && value.toLowerCase().contains(q);

    if (contains(pkg.packageName) ||
        contains(pkg.packageIdentityId) ||
        contains(pkg.installationIdentityId) ||
        contains(pkg.version) ||
        contains(pkg.architecture) ||
        contains(pkg.summary) ||
        contains(pkg.description) ||
        contains(pkg.section) ||
        contains(pkg.packageGroup) ||
        contains(pkg.license) ||
        contains(pkg.maintainer) ||
        contains(pkg.vendor) ||
        contains(pkg.profilePath)) {
      return true;
    }
    for (final g in pkg.groups) {
      if (g.toLowerCase().contains(q)) return true;
    }
    for (final l in pkg.licenses) {
      if (l.toLowerCase().contains(q)) return true;
    }
    return false;
  }

  int _comparePackages(LinuxInstalledPackage a, LinuxInstalledPackage b) {
    final nameA = a.packageName.toLowerCase();
    final nameB = b.packageName.toLowerCase();
    return switch (_packageSort) {
      PackageSortOption.nameAsc =>
        nameA.compareTo(nameB) != 0
            ? nameA.compareTo(nameB)
            : packageUniqueKey(a).compareTo(packageUniqueKey(b)),
      PackageSortOption.nameDesc =>
        nameB.compareTo(nameA) != 0
            ? nameB.compareTo(nameA)
            : packageUniqueKey(b).compareTo(packageUniqueKey(a)),
      PackageSortOption.source =>
        a.provenance.installationSource.name.compareTo(
                  b.provenance.installationSource.name,
                ) !=
                0
            ? a.provenance.installationSource.name.compareTo(
                b.provenance.installationSource.name,
              )
            : nameA.compareTo(nameB),
      PackageSortOption.version =>
        (a.version ?? '').compareTo(b.version ?? '') != 0
            ? (a.version ?? '').compareTo(b.version ?? '')
            : nameA.compareTo(nameB),
    };
  }
}
