## 1.1.0

- Added `FlutterDeviceAppsLinux.discoverApps()` for normalized, deduplicated Linux application discovery across local sources (GIO, Flatpak, Snap, DPKG, RPM, Pacman, Nix, and AppStream metadata).
- Added strongly typed, immutable discovery models for applications, desktop entries, installed metadata, upstream catalog releases, provenance, tri-state capabilities, classification, metadata conflicts, correlation ambiguities, provider diagnostics, and optional installed package inventory (`includePackages: true`).
- Added optional runtime loading via `GModule` for `libflatpak`, `librpm`, `libalpm`, and `libappstream`, plus read-only local `snapd` UNIX socket, DPKG status/file-ownership, and Nix profile discovery without invoking package manager CLIs.
- Preserved full backward compatibility for `listApps()` (GIO-only) and all existing `1.0.0` APIs.

## 1.0.0

- Added desktop ID lookup, app search, launch, and PNG icons.
- Added desktop entry details, file and URI support flags, and terminal launch information.
- Added default and registered handler lookups for local files and MIME types.
- Added local AppStream metadata and Flatpak, Snap, and AppImage indicators.
- Added categorized technology detection from direct ELF dependencies.
- Added the Linux example, API guide, and release workflows.

## 0.0.1

- Added `FlutterDeviceAppsLinux.listApps()` to list visible Linux desktop apps with their names and desktop file IDs.
