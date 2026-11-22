part of 'flutter_device_apps_linux.dart';

/// A technology identified by a direct ELF dependency.
enum LinuxAppTechnology {
  flutter({_TechnologyCategory.framework, _TechnologyCategory.uiToolkit}),
  qt({_TechnologyCategory.framework, _TechnologyCategory.library}),
  qtWidgets({_TechnologyCategory.uiToolkit}),
  qtQuick({_TechnologyCategory.uiToolkit}),
  gtk3({_TechnologyCategory.uiToolkit}),
  gtk4({_TechnologyCategory.uiToolkit}),
  libadwaita({_TechnologyCategory.uiToolkit}),
  sdl2({_TechnologyCategory.library}),
  sdl3({_TechnologyCategory.library}),
  webKitGtk({_TechnologyCategory.library}),
  gstreamer({_TechnologyCategory.library}),
  libVlc({_TechnologyCategory.library}),
  libMpv({_TechnologyCategory.library}),
  openGL({_TechnologyCategory.library}),
  vulkan({_TechnologyCategory.library});

  const LinuxAppTechnology(this._categories);

  final Set<_TechnologyCategory> _categories;
}

enum _TechnologyCategory { framework, uiToolkit, library }

/// Categorized technologies found in an ELF executable's direct dependencies.
class LinuxAppTechnologies {
  const LinuxAppTechnologies._(
    this.frameworks,
    this.uiToolkits,
    this.libraries,
  );

  factory LinuxAppTechnologies._fromDependencies(List<String> dependencies) {
    final matches = {
      for (final dependency in dependencies)
        ?_librariesByName[_soname.firstMatch(dependency.split('/').last)?[1]],
    };
    List<LinuxAppTechnology> forCategory(_TechnologyCategory category) =>
        List.unmodifiable(
          LinuxAppTechnology.values.where(
            (technology) =>
                matches.contains(technology) &&
                technology._categories.contains(category),
          ),
        );
    return LinuxAppTechnologies._(
      forCategory(_TechnologyCategory.framework),
      forCategory(_TechnologyCategory.uiToolkit),
      forCategory(_TechnologyCategory.library),
    );
  }

  static final _soname = RegExp(r'^(.+)\.so(?:\.[0-9]+)*$');
  static const _librariesByName = {
    'libflutter_linux_gtk': LinuxAppTechnology.flutter,
    'libQt5Core': LinuxAppTechnology.qt,
    'libQt6Core': LinuxAppTechnology.qt,
    'libQt5Widgets': LinuxAppTechnology.qtWidgets,
    'libQt6Widgets': LinuxAppTechnology.qtWidgets,
    'libQt5Quick': LinuxAppTechnology.qtQuick,
    'libQt6Quick': LinuxAppTechnology.qtQuick,
    'libgtk-3': LinuxAppTechnology.gtk3,
    'libgtk-4': LinuxAppTechnology.gtk4,
    'libadwaita-1': LinuxAppTechnology.libadwaita,
    'libSDL2-2.0': LinuxAppTechnology.sdl2,
    'libSDL2': LinuxAppTechnology.sdl2,
    'libSDL3': LinuxAppTechnology.sdl3,
    'libwebkit2gtk-4.0': LinuxAppTechnology.webKitGtk,
    'libwebkit2gtk-4.1': LinuxAppTechnology.webKitGtk,
    'libwebkitgtk-6.0': LinuxAppTechnology.webKitGtk,
    'libgstreamer-1.0': LinuxAppTechnology.gstreamer,
    'libvlc': LinuxAppTechnology.libVlc,
    'libmpv': LinuxAppTechnology.libMpv,
    'libGL': LinuxAppTechnology.openGL,
    'libOpenGL': LinuxAppTechnology.openGL,
    'libvulkan': LinuxAppTechnology.vulkan,
  };

  final List<LinuxAppTechnology> frameworks;
  final List<LinuxAppTechnology> uiToolkits;
  final List<LinuxAppTechnology> libraries;
}
