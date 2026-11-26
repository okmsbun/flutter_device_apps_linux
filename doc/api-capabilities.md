# List installed Linux apps in Flutter

`flutter_device_apps_linux` uses GIO to discover and inspect Linux desktop
applications. Each result is identified by its desktop file ID, such as
`org.gnome.Nautilus.desktop`.

## Discovery and lookup

`listApps()` returns desktop entries that GIO considers visible for the current
desktop session. `searchApps()` uses GIO's ranked search, keeps visible entries,
and returns an empty list for a blank query. GIO resolves entries using the
session's XDG data directories. Apps installed without a registered desktop
entry may be absent. [Desktop Entry Specification](https://specifications.freedesktop.org/desktop-entry/latest/)

`getApp()` resolves one desktop ID independently of menu visibility. It returns
`null` when GIO cannot resolve it. The three capability flags describe files,
URIs, and a terminal request as declared by the entry.

## File handlers and launch

Use `getDefaultAppForFile()` and `listAppsForFile()` with an absolute local
path. The MIME variants resolve the desktop associations for a value such as
`application/pdf`. Missing associations yield `null` for a default lookup
and `[]` for a list lookup. A missing file yields the same results.

`openApp()` returns whether GIO accepted the launch request. A successful
request can still be followed by an app startup failure. `getAppIcon()`
loads the icon through the active GTK theme and returns PNG bytes when available.
[GIO AppInfo](https://docs.gtk.org/gio/iface.AppInfo.html)

## AppStream and packaging metadata

`getAppMetadata()` looks for matching local `.metainfo.xml` and
`.appdata.xml` files. The AppStream desktop launchable ID associates the
record with an app. Legacy records can match by component ID. The method reads
localized text where available and returns `null` if no readable record matches.
[AppStream desktop applications](https://www.freedesktop.org/software/appstream/docs/sect-Metadata-Application.html)

`getInstallSourceInfo()` uses desktop entry markers for Flatpak, Snap, and
AppImage. The source is `unknown` when an entry has no supported marker.
Its package ID and version are optional.

## Unified multi-source discovery (`discoverApps`)

`FlutterDeviceAppsLinux.discoverApps()` discovers, correlates, and deduplicates
local installations across available read-only providers without invoking
package manager CLIs or network APIs:

```dart
final result = await FlutterDeviceAppsLinux.discoverApps(
  includePackages: false,
  includeAppStreamMetadata: true,
  providers: null, // or a non-empty list such as ['gio', 'flatpak', 'dpkg']
);
```

### Options

- `includePackages` (default `false`): when `true`, populates
  `LinuxDiscoveryResult.packages` with raw installed system/profile packages
  kept strictly separate from confirmed `applications`.
- `includeAppStreamMetadata` (default `true`): controls local AppStream
  enrichment (`libappstream`).
- `providers` (default `null`): when `null`, queries all registered local
  providers; when provided, must be a non-empty list of non-empty provider IDs
  without NUL characters (otherwise throws `ArgumentError`).

### Local providers and optional dependencies

- **Application & desktop providers:**
  - `gio`: XDG `.desktop` entries via GIO (always available).
  - `flatpak`: user and system Flatpak installations via optional `libflatpak`
    loaded dynamically with `GModule`.
  - `snap`: enabled Snap applications via the local `snapd` UNIX socket.
- **Package inventory & file ownership:**
  - `dpkg`: local `/var/lib/dpkg` status and file-ownership inventories.
  - `rpm`: local RPM database and `rpmfi` full-path file ownership via optional
    `librpm` loaded dynamically with `GModule`.
  - `pacman`: local Pacman database and file lists via optional `libalpm`
    loaded dynamically with `GModule`.
  - `nix`: active local Nix profile manifests and verified store paths.
- **Metadata enrichment:**
  - `appstream`: local AppStream MetaInfo and catalog metadata via optional
    `libappstream` loaded dynamically with `GModule`.

### Typed models and provider diagnostics

All returned models and collections are immutable:

- `LinuxDiscoveryResult`: `applications` (`List<LinuxDiscoveredApp>`),
  `packages` (`List<LinuxInstalledPackage>`), `providerDiagnostics`
  (`List<LinuxDiscoveryProviderDiagnostic>`), and top-level `ambiguities`
  (`List<LinuxResolutionAmbiguity>`).
- `LinuxDiscoveredApp`: `resolvedId`, `primaryProvenance`, `sourceProvenances`,
  `originalRecordIds`, `desktopEntries` (`List<LinuxResolvedDesktopEntry>`),
  `linkedPackageIdentityIds`, `linkedAppStreamComponentIds`, `metadata`
  (`LinuxDiscoveredAppMetadata`), `catalogReleases` (`LinuxCatalogReleaseInfo`
  with `List<LinuxCatalogRelease>`), `capabilities` (`LinuxAppCapabilities`),
  `desktopMenuVisibility`, `classification`, `metadataConflicts`
  (`List<LinuxMetadataConflict>`), and `ambiguities`.
- `LinuxDiscoveryProviderDiagnostic`: reports `providerId`, `discoveredCount`,
  optional `errorMessage`, and `status` (`succeeded`, `partiallyFailed`,
  `unavailable`, or `failed`). Missing optional libraries, databases, or
  daemons report `unavailable` without failing the overall pass; isolated
  provider failures do not discard healthy results from other providers.

### Semantics and guarantees

- **Applications vs. packages:** Confirmed applications are returned in
  `LinuxDiscoveryResult.applications`. Raw system and profile packages remain
  strictly separate in `LinuxDiscoveryResult.packages` (empty unless
  `includePackages: true`). Catalog-only AppStream components and arbitrary
  installed packages are never promoted into applications.
- **Installed vs. catalog versions:** `LinuxDiscoveredAppMetadata.version` only
  contains a verified installed version reported by an installation provider.
  Upstream AppStream release history is kept separate in
  `LinuxDiscoveredApp.catalogReleases`.
- **Desktop entries:** Desktop entries are never fabricated. An application
  without a `.desktop` file has an empty `desktopEntries` list; an application
  that owns multiple `.desktop` files exposes all of them.
- **Honest unknowns & provenance:** Capabilities use tri-state
  `LinuxCapabilityState` (`supported`, `unsupported`, `unknown`). Classification
  remains `LinuxAppClassification.unknown` unless authoritative metadata (such
  as strongly matched AppStream component kinds) establishes it. Unresolved
  matches and field disagreements are preserved in `ambiguities` and
  `metadataConflicts` instead of guessing.
- **Separate installations:** Installations across different package managers,
  scopes (user/system), Flatpak branches, architectures, Snap parallel
  instances, Nix profiles, or parallel RPM versions remain distinct
  `LinuxDiscoveredApp` records.

### Known limitations and identity stability

- **Deterministic identity:** `LinuxDiscoveredApp.resolvedId` is deterministic
  and independent of provider execution order. For a normal single installation
  (GIO, Flatpak, Snap, DPKG, Pacman, Nix, or single-version RPM), `resolvedId`
  remains stable across in-place upgrades.
- **Parallel RPM transition:** When an RPM package transitions between a single
  installed version and simultaneous multi-version (parallel) installations,
  parallel records must use version-specific installation identities so each
  installed version remains distinct; consequently, `resolvedId` changes across
  that single-to-parallel transition.
- **Cross-provider / cross-scope moves:** Replacing an application with another
  packaging format (for example, DPKG to Flatpak) or moving between scopes or
  Nix profiles produces a distinct `resolvedId` by design.
- **Sandboxed or restricted environments:** Inside strict sandboxes (such as
  Flatpak or confined Snaps) or when host databases/sockets are unreadable,
  affected providers report `unavailable` or `failed` in `providerDiagnostics`.
- **No new launch mechanisms:** Launching remains handled by `openApp()` using
  a valid GIO `desktopFileId`; applications discovered without a desktop entry
  cannot be launched through `openApp()`.

## Technology detection

`detectAppTechnologies()` reads direct ELF dependency metadata from an
accessible executable. The executable is inspected without launching it.
Relevant dependencies can appear in more than one category. Apps launched
through a script or packaged wrapper can produce empty categories.
[ELF shared object dependencies](https://gabi.xinuos.com/elf/09-dynamic.html#shared-object-dependencies)
