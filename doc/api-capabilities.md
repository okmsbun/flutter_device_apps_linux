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

`discoverApps()` discovers and correlates local installations across available
read-only providers without invoking package manager CLIs or network APIs:

- **Application & desktop providers:** GIO desktop entries, Flatpak (`libflatpak`
  loaded optionally at runtime), and Snap (local `snapd` UNIX socket).
- **Package inventory & file ownership:** DPKG (`/var/lib/dpkg`), RPM (`librpm`
  loaded optionally at runtime), Pacman (`libalpm` loaded optionally at runtime),
  and active local Nix profiles.
- **Metadata enrichment:** Local AppStream metadata (`libappstream` loaded
  optionally at runtime, disabled with `includeAppStreamMetadata: false`).

Missing optional libraries, databases, or daemons are reported as
`LinuxDiscoveryProviderStatus.unavailable` in `providerDiagnostics` without
failing the overall pass.

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

### Identity stability limitations

- `LinuxDiscoveredApp.resolvedId` is deterministic and independent of provider
  execution order.
- For a normal single installation (GIO, Flatpak, Snap, DPKG, Pacman, Nix, or
  single-version RPM), `resolvedId` remains stable across in-place upgrades.
- **Parallel RPM transition:** When an RPM package transitions between a single
  installed version and simultaneous multi-version (parallel) installations,
  parallel records must use version-specific installation identities so each
  installed version remains distinct; consequently, `resolvedId` changes across
  that single-to-parallel transition.
- **Cross-provider / cross-scope moves:** Replacing a package with another
  packaging format (for example, DPKG to Flatpak) or moving between scopes or
  Nix profiles produces a distinct `resolvedId` by design.

## Technology detection

`detectAppTechnologies()` reads direct ELF dependency metadata from an
accessible executable. The executable is inspected without launching it.
Relevant dependencies can appear in more than one category. Apps launched
through a script or packaged wrapper can produce empty categories.
[ELF shared object dependencies](https://gabi.xinuos.com/elf/09-dynamic.html#shared-object-dependencies)
