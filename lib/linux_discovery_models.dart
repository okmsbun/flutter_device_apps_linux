part of 'flutter_device_apps_linux.dart';

/// Known packaging or installation source recorded for a discovered item.
///
/// Remains [unknown] when no authoritative installation provider verified the
/// source (for example, standalone desktop entries without package ownership).
enum LinuxInstallationSource {
  unknown,
  flatpak,
  snap,
  appImage,
  dpkg,
  rpm,
  pacman,
  nix,
}

/// Scope in which an application or package is installed.
enum LinuxInstallationScope { unknown, system, user, other }

/// Tri-state capability value that keeps [unknown] distinct from [unsupported].
enum LinuxCapabilityState { unknown, supported, unsupported }

/// Desktop menu visibility state as reported by GIO (`g_app_info_should_show`).
///
/// Kept distinct from installation status, launchability, and classification.
enum LinuxDesktopMenuVisibility { unknown, visible, notVisible }

/// Authoritative classification of a discovered application or package.
///
/// Remains [unknown] unless authoritative metadata (such as strongly matched
/// AppStream component kinds) establishes the classification.
enum LinuxAppClassification {
  unknown,
  guiApplication,
  cliApplication,
  service,
  library,
  runtime,
}

/// Execution outcome of an individual local discovery provider or ownership
/// indexer during a unified discovery pass.
enum LinuxDiscoveryProviderStatus {
  succeeded,
  partiallyFailed,
  unavailable,
  failed,
}

/// Actual installation state reported by a system package manager.
enum LinuxPackageInstallationState {
  unknown,
  installed,
  triggersAwaited,
  triggersPending,
}

/// Desired selection state recorded by a package manager (for example, DPKG).
enum LinuxPackageSelectionState { unknown, install, hold, deinstall, purge }

/// Authoritative installation reason recorded by a package manager (for
/// example, Pacman/libalpm explicit versus dependency).
enum LinuxPackageInstallReason { unknown, explicit, dependency }

T _enumByName<T extends Enum>(List<T> values, Object? raw, T fallback) {
  if (raw is! String) return fallback;
  for (final value in values) {
    if (value.name == raw) return value;
  }
  return fallback;
}

List<String> _stringList(Object? raw) {
  if (raw is! List<Object?>) return const [];
  return List<String>.unmodifiable(raw.whereType<String>());
}

/// Provenance describing which local provider discovered a record.
@immutable
class LinuxSourceProvenance {
  const LinuxSourceProvenance({
    required this.providerId,
    required this.nativeId,
    this.installationSource = LinuxInstallationSource.unknown,
    this.installationScope = LinuxInstallationScope.unknown,
  });

  factory LinuxSourceProvenance._fromMap(Map<Object?, Object?> map) =>
      LinuxSourceProvenance(
        providerId: map['providerId'] as String? ?? '',
        nativeId: map['nativeId'] as String? ?? '',
        installationSource: _enumByName(
          LinuxInstallationSource.values,
          map['installationSource'],
          LinuxInstallationSource.unknown,
        ),
        installationScope: _enumByName(
          LinuxInstallationScope.values,
          map['installationScope'],
          LinuxInstallationScope.unknown,
        ),
      );

  /// Identifier of the local provider (such as `gio`, `flatpak`, `snap`,
  /// `dpkg`, `rpm`, `pacman`, or `nix`).
  final String providerId;

  /// Provider-native identifier before provider scoping.
  final String nativeId;

  /// Verified packaging or installation source, or [LinuxInstallationSource.unknown].
  final LinuxInstallationSource installationSource;

  /// Installation scope, or [LinuxInstallationScope.unknown].
  final LinuxInstallationScope installationScope;
}

/// Tri-state application capabilities.
@immutable
class LinuxAppCapabilities {
  const LinuxAppCapabilities({
    this.supportsFiles = LinuxCapabilityState.unknown,
    this.supportsUris = LinuxCapabilityState.unknown,
    this.requiresTerminal = LinuxCapabilityState.unknown,
    this.launchable = LinuxCapabilityState.unknown,
  });

  factory LinuxAppCapabilities._fromMap(Map<Object?, Object?>? map) {
    if (map == null) return const LinuxAppCapabilities();
    return LinuxAppCapabilities(
      supportsFiles: _enumByName(
        LinuxCapabilityState.values,
        map['supportsFiles'],
        LinuxCapabilityState.unknown,
      ),
      supportsUris: _enumByName(
        LinuxCapabilityState.values,
        map['supportsUris'],
        LinuxCapabilityState.unknown,
      ),
      requiresTerminal: _enumByName(
        LinuxCapabilityState.values,
        map['requiresTerminal'],
        LinuxCapabilityState.unknown,
      ),
      launchable: _enumByName(
        LinuxCapabilityState.values,
        map['launchable'],
        LinuxCapabilityState.unknown,
      ),
    );
  }

  final LinuxCapabilityState supportsFiles;
  final LinuxCapabilityState supportsUris;
  final LinuxCapabilityState requiresTerminal;
  final LinuxCapabilityState launchable;
}

/// An authoritative desktop entry correlated with a [LinuxDiscoveredApp].
///
/// Desktop entries are never fabricated; applications installed without a
/// `.desktop` file expose an empty [LinuxDiscoveredApp.desktopEntries] list.
@immutable
class LinuxResolvedDesktopEntry {
  const LinuxResolvedDesktopEntry({
    required this.desktopFileId,
    this.desktopFilePath,
    this.displayName,
    this.desktopMenuVisibility = LinuxDesktopMenuVisibility.unknown,
    this.capabilities = const LinuxAppCapabilities(),
    required this.providerId,
    required this.originalApplicationId,
  });

  factory LinuxResolvedDesktopEntry._fromMap(Map<Object?, Object?> map) =>
      LinuxResolvedDesktopEntry(
        desktopFileId: map['desktopFileId'] as String? ?? '',
        desktopFilePath: map['desktopFilePath'] as String?,
        displayName: map['displayName'] as String?,
        desktopMenuVisibility: _enumByName(
          LinuxDesktopMenuVisibility.values,
          map['desktopMenuVisibility'],
          LinuxDesktopMenuVisibility.unknown,
        ),
        capabilities: LinuxAppCapabilities._fromMap(
          map['capabilities'] as Map<Object?, Object?>?,
        ),
        providerId: map['providerId'] as String? ?? '',
        originalApplicationId: map['originalApplicationId'] as String? ?? '',
      );

  /// The `.desktop` entry identifier, such as `org.gnome.Nautilus.desktop`.
  final String desktopFileId;

  /// Absolute path to the `.desktop` file when reported by GIO.
  final String? desktopFilePath;

  /// Display name declared by this desktop entry, when available.
  final String? displayName;

  /// Desktop menu visibility state for this entry.
  final LinuxDesktopMenuVisibility desktopMenuVisibility;

  /// Capabilities declared by this desktop entry.
  final LinuxAppCapabilities capabilities;

  /// Provider that discovered this desktop entry (typically `gio`).
  final String providerId;

  /// Original provider-scoped record ID prior to correlation.
  final String originalApplicationId;
}

/// Installed descriptive metadata for a [LinuxDiscoveredApp].
///
/// [version] represents ONLY the verified installed version reported by an
/// installation provider (never an upstream AppStream catalog release version;
/// see [LinuxDiscoveredApp.catalogReleases]).
@immutable
class LinuxDiscoveredAppMetadata {
  const LinuxDiscoveredAppMetadata({
    this.name,
    this.version,
    this.revision,
    this.channel,
    this.summary,
    this.description,
    this.executable,
    this.homepage,
    this.license,
    this.iconReference,
    this.architecture,
    this.branch,
  });

  factory LinuxDiscoveredAppMetadata._fromMap(Map<Object?, Object?>? map) {
    if (map == null) return const LinuxDiscoveredAppMetadata();
    return LinuxDiscoveredAppMetadata(
      name: map['name'] as String?,
      version: map['version'] as String?,
      revision: map['revision'] as String?,
      channel: map['channel'] as String?,
      summary: map['summary'] as String?,
      description: map['description'] as String?,
      executable: map['executable'] as String?,
      homepage: map['homepage'] as String?,
      license: map['license'] as String?,
      iconReference: map['iconReference'] as String?,
      architecture: map['architecture'] as String?,
      branch: map['branch'] as String?,
    );
  }

  final String? name;

  /// Verified installed version from an installation provider, or `null`.
  final String? version;

  final String? revision;
  final String? channel;
  final String? summary;
  final String? description;
  final String? executable;
  final String? homepage;
  final String? license;
  final String? iconReference;
  final String? architecture;
  final String? branch;
}

/// A single upstream release entry from local AppStream metadata.
///
/// Kept strictly separate from [LinuxDiscoveredAppMetadata.version].
@immutable
class LinuxCatalogRelease {
  const LinuxCatalogRelease({
    this.version,
    this.date,
    this.timestamp,
    this.urgency,
  });

  factory LinuxCatalogRelease._fromMap(Map<Object?, Object?> map) =>
      LinuxCatalogRelease(
        version: map['version'] as String?,
        date: map['date'] as String?,
        timestamp: (map['timestamp'] as num?)?.toInt(),
        urgency: map['urgency'] as String?,
      );

  final String? version;
  final String? date;
  final int? timestamp;
  final String? urgency;
}

/// Upstream AppStream catalog/metainfo release metadata for an application.
@immutable
class LinuxCatalogReleaseInfo {
  const LinuxCatalogReleaseInfo({
    this.latestVersion,
    this.latestDate,
    this.releases = const [],
  });

  factory LinuxCatalogReleaseInfo._fromMap(Map<Object?, Object?>? map) {
    if (map == null) return const LinuxCatalogReleaseInfo();
    final rawReleases = map['releases'] as List<Object?>? ?? const [];
    return LinuxCatalogReleaseInfo(
      latestVersion: map['latestVersion'] as String?,
      latestDate: map['latestDate'] as String?,
      releases: List<LinuxCatalogRelease>.unmodifiable([
        for (final item in rawReleases)
          if (item is Map<Object?, Object?>) LinuxCatalogRelease._fromMap(item),
      ]),
    );
  }

  final String? latestVersion;
  final String? latestDate;
  final List<LinuxCatalogRelease> releases;
}

/// A preserved field-level metadata disagreement between authoritative sources.
@immutable
class LinuxMetadataConflict {
  const LinuxMetadataConflict({
    required this.fieldName,
    required this.chosenProviderId,
    required this.chosenValue,
    required this.conflictingProviderId,
    required this.conflictingValue,
  });

  factory LinuxMetadataConflict._fromMap(Map<Object?, Object?> map) =>
      LinuxMetadataConflict(
        fieldName: map['fieldName'] as String? ?? '',
        chosenProviderId: map['chosenProviderId'] as String? ?? '',
        chosenValue: map['chosenValue'] as String? ?? '',
        conflictingProviderId: map['conflictingProviderId'] as String? ?? '',
        conflictingValue: map['conflictingValue'] as String? ?? '',
      );

  final String fieldName;
  final String chosenProviderId;
  final String chosenValue;
  final String conflictingProviderId;
  final String conflictingValue;
}

/// An ambiguity preserved during correlation instead of guessing.
@immutable
class LinuxResolutionAmbiguity {
  const LinuxResolutionAmbiguity({
    required this.reason,
    this.candidateIds = const [],
  });

  factory LinuxResolutionAmbiguity._fromMap(Map<Object?, Object?> map) =>
      LinuxResolutionAmbiguity(
        reason: map['reason'] as String? ?? '',
        candidateIds: _stringList(map['candidateIds']),
      );

  final String reason;
  final List<String> candidateIds;
}

/// Per-provider execution status and diagnostics for a discovery pass.
@immutable
class LinuxDiscoveryProviderDiagnostic {
  const LinuxDiscoveryProviderDiagnostic({
    required this.providerId,
    required this.status,
    this.discoveredCount = 0,
    this.errorMessage,
  });

  factory LinuxDiscoveryProviderDiagnostic._fromMap(
    Map<Object?, Object?> map,
  ) => LinuxDiscoveryProviderDiagnostic(
    providerId: map['providerId'] as String? ?? '',
    status: _enumByName(
      LinuxDiscoveryProviderStatus.values,
      map['status'],
      LinuxDiscoveryProviderStatus.failed,
    ),
    discoveredCount: (map['discoveredCount'] as num?)?.toInt() ?? 0,
    errorMessage: map['errorMessage'] as String?,
  );

  final String providerId;
  final LinuxDiscoveryProviderStatus status;
  final int discoveredCount;
  final String? errorMessage;
}

/// An installed system or profile package record kept separate from confirmed
/// applications.
@immutable
class LinuxInstalledPackage {
  const LinuxInstalledPackage({
    required this.packageIdentityId,
    this.installationIdentityId,
    required this.provenance,
    required this.packageName,
    this.architecture,
    this.epoch,
    this.version,
    this.release,
    this.summary,
    this.description,
    this.section,
    this.packageGroup,
    this.groups = const [],
    this.priority,
    this.license,
    this.licenses = const [],
    this.homepage,
    this.maintainer,
    this.packager,
    this.vendor,
    this.profilePath,
    this.attributePath,
    this.sourceUrl,
    this.lockedUrl,
    this.outputs = const [],
    this.storePaths = const [],
    this.installPriority,
    this.installationState = LinuxPackageInstallationState.unknown,
    this.selectionState = LinuxPackageSelectionState.unknown,
    this.installReason = LinuxPackageInstallReason.unknown,
    this.classification = LinuxAppClassification.unknown,
  });

  factory LinuxInstalledPackage._fromMap(Map<Object?, Object?> map) =>
      LinuxInstalledPackage(
        packageIdentityId: map['packageIdentityId'] as String? ?? '',
        installationIdentityId: map['installationIdentityId'] as String?,
        provenance: LinuxSourceProvenance._fromMap(
          map['provenance'] as Map<Object?, Object?>? ?? const {},
        ),
        packageName: map['packageName'] as String? ?? '',
        architecture: map['architecture'] as String?,
        epoch: map['epoch'] as String?,
        version: map['version'] as String?,
        release: map['release'] as String?,
        summary: map['summary'] as String?,
        description: map['description'] as String?,
        section: map['section'] as String?,
        packageGroup: map['packageGroup'] as String?,
        groups: _stringList(map['groups']),
        priority: map['priority'] as String?,
        license: map['license'] as String?,
        licenses: _stringList(map['licenses']),
        homepage: map['homepage'] as String?,
        maintainer: map['maintainer'] as String?,
        packager: map['packager'] as String?,
        vendor: map['vendor'] as String?,
        profilePath: map['profilePath'] as String?,
        attributePath: map['attributePath'] as String?,
        sourceUrl: map['sourceUrl'] as String?,
        lockedUrl: map['lockedUrl'] as String?,
        outputs: _stringList(map['outputs']),
        storePaths: _stringList(map['storePaths']),
        installPriority: (map['installPriority'] as num?)?.toInt(),
        installationState: _enumByName(
          LinuxPackageInstallationState.values,
          map['installationState'],
          LinuxPackageInstallationState.unknown,
        ),
        selectionState: _enumByName(
          LinuxPackageSelectionState.values,
          map['selectionState'],
          LinuxPackageSelectionState.unknown,
        ),
        installReason: _enumByName(
          LinuxPackageInstallReason.values,
          map['installReason'],
          LinuxPackageInstallReason.unknown,
        ),
        classification: _enumByName(
          LinuxAppClassification.values,
          map['classification'],
          LinuxAppClassification.unknown,
        ),
      );

  /// Provider-scoped package identity stable across normal version upgrades.
  final String packageIdentityId;

  /// Version-specific identity when a package manager permits parallel
  /// installed versions (such as RPM kernels).
  final String? installationIdentityId;

  final LinuxSourceProvenance provenance;
  final String packageName;
  final String? architecture;
  final String? epoch;
  final String? version;
  final String? release;
  final String? summary;
  final String? description;
  final String? section;
  final String? packageGroup;
  final List<String> groups;
  final String? priority;
  final String? license;
  final List<String> licenses;
  final String? homepage;
  final String? maintainer;
  final String? packager;
  final String? vendor;
  final String? profilePath;
  final String? attributePath;
  final String? sourceUrl;
  final String? lockedUrl;
  final List<String> outputs;
  final List<String> storePaths;
  final int? installPriority;
  final LinuxPackageInstallationState installationState;
  final LinuxPackageSelectionState selectionState;
  final LinuxPackageInstallReason installReason;
  final LinuxAppClassification classification;
}

/// A normalized, deduplicated, and enriched Linux application record returned
/// by [FlutterDeviceAppsLinux.discoverApps].
///
/// Separate installations across package managers, scopes, branches,
/// architectures, parallel Snap instances, parallel RPM versions, or Nix
/// profiles remain distinct [LinuxDiscoveredApp] entries.
@immutable
class LinuxDiscoveredApp {
  const LinuxDiscoveredApp({
    required this.resolvedId,
    required this.primaryProvenance,
    this.sourceProvenances = const [],
    this.originalRecordIds = const [],
    this.desktopEntries = const [],
    this.linkedPackageIdentityIds = const [],
    this.linkedAppStreamComponentIds = const [],
    this.metadata = const LinuxDiscoveredAppMetadata(),
    this.catalogReleases = const LinuxCatalogReleaseInfo(),
    this.capabilities = const LinuxAppCapabilities(),
    this.desktopMenuVisibility = LinuxDesktopMenuVisibility.unknown,
    this.classification = LinuxAppClassification.unknown,
    this.metadataConflicts = const [],
    this.ambiguities = const [],
  });

  factory LinuxDiscoveredApp._fromMap(Map<Object?, Object?> map) {
    final rawProvs = map['sourceProvenances'] as List<Object?>? ?? const [];
    final rawDesktops = map['desktopEntries'] as List<Object?>? ?? const [];
    final rawConflicts = map['metadataConflicts'] as List<Object?>? ?? const [];
    final rawAmbiguities = map['ambiguities'] as List<Object?>? ?? const [];
    return LinuxDiscoveredApp(
      resolvedId: map['resolvedId'] as String? ?? '',
      primaryProvenance: LinuxSourceProvenance._fromMap(
        map['primaryProvenance'] as Map<Object?, Object?>? ?? const {},
      ),
      sourceProvenances: List<LinuxSourceProvenance>.unmodifiable([
        for (final item in rawProvs)
          if (item is Map<Object?, Object?>)
            LinuxSourceProvenance._fromMap(item),
      ]),
      originalRecordIds: _stringList(map['originalRecordIds']),
      desktopEntries: List<LinuxResolvedDesktopEntry>.unmodifiable([
        for (final item in rawDesktops)
          if (item is Map<Object?, Object?>)
            LinuxResolvedDesktopEntry._fromMap(item),
      ]),
      linkedPackageIdentityIds: _stringList(map['linkedPackageIdentityIds']),
      linkedAppStreamComponentIds: _stringList(
        map['linkedAppStreamComponentIds'],
      ),
      metadata: LinuxDiscoveredAppMetadata._fromMap(
        map['metadata'] as Map<Object?, Object?>?,
      ),
      catalogReleases: LinuxCatalogReleaseInfo._fromMap(
        map['catalogReleases'] as Map<Object?, Object?>?,
      ),
      capabilities: LinuxAppCapabilities._fromMap(
        map['capabilities'] as Map<Object?, Object?>?,
      ),
      desktopMenuVisibility: _enumByName(
        LinuxDesktopMenuVisibility.values,
        map['desktopMenuVisibility'],
        LinuxDesktopMenuVisibility.unknown,
      ),
      classification: _enumByName(
        LinuxAppClassification.values,
        map['classification'],
        LinuxAppClassification.unknown,
      ),
      metadataConflicts: List<LinuxMetadataConflict>.unmodifiable([
        for (final item in rawConflicts)
          if (item is Map<Object?, Object?>)
            LinuxMetadataConflict._fromMap(item),
      ]),
      ambiguities: List<LinuxResolutionAmbiguity>.unmodifiable([
        for (final item in rawAmbiguities)
          if (item is Map<Object?, Object?>)
            LinuxResolutionAmbiguity._fromMap(item),
      ]),
    );
  }

  /// Deterministic canonical identifier for this verified installation.
  final String resolvedId;

  /// Primary installation provenance.
  final LinuxSourceProvenance primaryProvenance;

  /// All contributing provider provenances merged into this record.
  final List<LinuxSourceProvenance> sourceProvenances;

  /// Original provider-scoped record identifiers prior to correlation.
  final List<String> originalRecordIds;

  /// Correlated `.desktop` entries (may be empty, single, or multiple).
  final List<LinuxResolvedDesktopEntry> desktopEntries;

  /// Linked installed package identities when verified via ownership evidence.
  final List<String> linkedPackageIdentityIds;

  /// Strongly matched AppStream component IDs.
  final List<String> linkedAppStreamComponentIds;

  /// Installed application metadata (installed version lives here).
  final LinuxDiscoveredAppMetadata metadata;

  /// Upstream AppStream catalog/metainfo releases (never merged into
  /// [metadata.version]).
  final LinuxCatalogReleaseInfo catalogReleases;

  /// Aggregated capabilities across attached desktop entries.
  final LinuxAppCapabilities capabilities;

  /// Aggregated desktop menu visibility across attached desktop entries.
  final LinuxDesktopMenuVisibility desktopMenuVisibility;

  /// Authoritative classification, or [LinuxAppClassification.unknown].
  final LinuxAppClassification classification;

  /// Field-level metadata disagreements preserved between sources.
  final List<LinuxMetadataConflict> metadataConflicts;

  /// Record-specific correlation ambiguities preserved instead of guessing.
  final List<LinuxResolutionAmbiguity> ambiguities;
}

/// Complete result returned by [FlutterDeviceAppsLinux.discoverApps].
///
/// Confirmed [applications] and raw installed [packages] remain strictly
/// separate collections. [packages] is populated only when
/// `includePackages: true` is passed to [FlutterDeviceAppsLinux.discoverApps].
@immutable
class LinuxDiscoveryResult {
  const LinuxDiscoveryResult({
    this.applications = const [],
    this.packages = const [],
    this.providerDiagnostics = const [],
    this.ambiguities = const [],
  });

  factory LinuxDiscoveryResult._fromMap(Map<Object?, Object?>? map) {
    if (map == null) return const LinuxDiscoveryResult();
    final rawApps = map['applications'] as List<Object?>? ?? const [];
    final rawPkgs = map['packages'] as List<Object?>? ?? const [];
    final rawDiags = map['providerDiagnostics'] as List<Object?>? ?? const [];
    final rawAmbs = map['ambiguities'] as List<Object?>? ?? const [];
    return LinuxDiscoveryResult(
      applications: List<LinuxDiscoveredApp>.unmodifiable([
        for (final item in rawApps)
          if (item is Map<Object?, Object?>) LinuxDiscoveredApp._fromMap(item),
      ]),
      packages: List<LinuxInstalledPackage>.unmodifiable([
        for (final item in rawPkgs)
          if (item is Map<Object?, Object?>)
            LinuxInstalledPackage._fromMap(item),
      ]),
      providerDiagnostics: List<LinuxDiscoveryProviderDiagnostic>.unmodifiable([
        for (final item in rawDiags)
          if (item is Map<Object?, Object?>)
            LinuxDiscoveryProviderDiagnostic._fromMap(item),
      ]),
      ambiguities: List<LinuxResolutionAmbiguity>.unmodifiable([
        for (final item in rawAmbs)
          if (item is Map<Object?, Object?>)
            LinuxResolutionAmbiguity._fromMap(item),
      ]),
    );
  }

  /// Normalized, deduplicated, and enriched installed application records.
  final List<LinuxDiscoveredApp> applications;

  /// Installed system/profile package inventory, populated only when
  /// `includePackages: true` is requested.
  final List<LinuxInstalledPackage> packages;

  /// Per-provider execution status and partial-failure diagnostics.
  final List<LinuxDiscoveryProviderDiagnostic> providerDiagnostics;

  /// Global/cross-record correlation ambiguities preserved instead of guessing.
  final List<LinuxResolutionAmbiguity> ambiguities;
}
