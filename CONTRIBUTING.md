# Contributing

Report bugs and feature requests in [Issues](https://github.com/okmsbun/flutter_device_apps_linux/issues).
For bugs, include the package version, Linux distribution, and steps to
reproduce. Follow the [security policy](SECURITY.md) for vulnerabilities.

Open pull requests against `develop`. Include the change and the checks you ran.

```sh
flutter pub get
dart format .
flutter analyze
flutter test
```

On Linux, run `bash example/tool/test_linux.sh` for native integration checks.
