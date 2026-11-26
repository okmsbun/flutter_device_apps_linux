cmake_minimum_required(VERSION 3.10)
project(flutter_device_apps_linux LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(GMODULE REQUIRED IMPORTED_TARGET gmodule-2.0)

set(PLUGIN_NAME "flutter_device_apps_linux_plugin")
add_library(${PLUGIN_NAME} SHARED
  flutter_device_apps_linux_plugin.cc
  elf_dependencies.cc
  discovery/application_discovery_service.cc
  discovery/application_identity.cc
  discovery/async_discovery_runner.cc
  discovery/flatpak_discovery_provider.cc
  discovery/gio_discovery_provider.cc
  discovery/legacy_list_apps_adapter.cc
  discovery/libflatpak_backend.cc
  discovery/snap_discovery_provider.cc
  discovery/snapd_socket_backend.cc
)
apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden)
target_include_directories(${PLUGIN_NAME} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter PkgConfig::GTK PkgConfig::GIO_UNIX PkgConfig::GMODULE)

set(flutter_device_apps_linux_bundled_libraries "" PARENT_SCOPE)
