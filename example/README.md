# Linux Application Explorer (Example)

A Material 3 Linux desktop application explorer demonstrating
`FlutterDeviceAppsLinux.discoverApps()` alongside desktop launching, ELF
technology detection, and file/MIME handler lookup.

## Run on Linux

On a Linux desktop with [Flutter's Linux tooling](https://docs.flutter.dev/platform-integration/linux/setup):

```sh
flutter pub get
flutter run -d linux
```

## Features

- **Dashboard:** Live statistics from `LinuxDiscoveryResult` (applications,
  separate package inventory, active providers, conflicts, and ambiguities) with
  clickable source and classification breakdowns.
- **Applications:** Master-detail browser with instant local search, sorting,
  source/classification/scope/visibility filters, cached desktop icons,
  installed vs. upstream catalog versions, capabilities, provenance, ELF
  technology detection, and app launching.
- **Packages:** Separate installed package browser (`includePackages: true`)
  with search, source/scope filters, and detailed package metadata.
- **Handlers:** File path and MIME type handler lookup (`getDefaultAppForFile`,
  `listAppsForFile`, `getDefaultAppForMimeType`, `listAppsForMimeType`).
- **Diagnostics:** Per-provider execution statuses, partial-failure reporting,
  discovery options, metadata conflicts, and resolution ambiguities.
- **Themes:** Light, Dark, and System theme modes with responsive sidebar and
  compact bottom navigation.

## Integration tests

Run the Linux integration suite from this directory with:

```sh
bash tool/test_linux.sh
```

The script uses Xvfb and isolated temporary XDG directories with known desktop
entries and ELF fixtures.
