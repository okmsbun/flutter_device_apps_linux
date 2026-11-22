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

## Technology detection

`detectAppTechnologies()` reads direct ELF dependency metadata from an
accessible executable. The executable is inspected without launching it.
Relevant dependencies can appear in more than one category. Apps launched
through a script or packaged wrapper can produce empty categories.
[ELF shared object dependencies](https://gabi.xinuos.com/elf/09-dynamic.html#shared-object-dependencies)
