#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LIBFLATPAK_BACKEND_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LIBFLATPAK_BACKEND_H_

#include <memory>

#include "flatpak_discovery_provider.h"

namespace flutter_device_apps_linux {

// Creates a runtime-loaded libflatpak backend using GModule (libflatpak.so.0).
// Returns a valid backend instance even when libflatpak is not installed;
// is_available() reports whether the shared library and required symbols were
// resolved.
//
// Minimum supported libflatpak API:
// - Core discovery (required): libflatpak >= 0.8.0 (or older baseline via
//   flatpak_installation_new_system fallback when
//   flatpak_get_system_installations is unavailable).
// - AppStream appdata metadata (optional): libflatpak >= 1.1.2
//   (flatpak_installed_ref_get_appdata_name/summary/version).
// - Non-interactive guard (optional): libflatpak >= 1.1.1
//   (flatpak_installation_set_no_interaction).
std::unique_ptr<FlatpakBackend> create_libflatpak_backend(
    const char* library_name = "libflatpak.so.0");

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LIBFLATPAK_BACKEND_H_
