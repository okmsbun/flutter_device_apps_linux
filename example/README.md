# Linux development host

On a Linux desktop with [Flutter's Linux tooling](https://docs.flutter.dev/platform-integration/linux/setup), run

```sh
flutter pub get
flutter run -d linux
```

Search the launcher entries and select a result to inspect its details, icon,
AppStream metadata, packaging indicators, and technologies. Use **Find handlers**
to query an absolute file path or a MIME type.

Run the integration checks from this directory with `bash tool/test_linux.sh`.
They use Xvfb and temporary XDG directories with known desktop entries.
