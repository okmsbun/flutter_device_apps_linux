#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LIBAPPSTREAM_BACKEND_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LIBAPPSTREAM_BACKEND_H_

#include <memory>
#include <string>
#include <vector>

#include "appstream_metadata_provider.h"

namespace flutter_device_apps_linux {

struct AppStreamDataLocation {
  std::string path;
  AppStreamMetadataOriginKind origin_kind =
      AppStreamMetadataOriginKind::kMetainfo;
};

struct LibAppStreamConfig {
  // Ordered SONAME candidates to probe via GModule.
  // Default probes libappstream.so.5 (AppStream 1.0+ ABI) first, then
  // libappstream.so.4 (AppStream 0.15/0.16 ABI). Each SONAME uses its own
  // explicitly verified symbol table and ownership contract.
  std::vector<std::string> library_candidates = {
      "libappstream.so.5",
      "libappstream.so.4",
  };

  // When true (default), loads standard local OS AppStream locations.
  // Set false in isolated local fixture tests that provide explicit
  // extra_locations without touching host system catalogs.
  bool load_standard_locations = true;

  // Additional local metadata directories to register with AsPool.
  std::vector<AppStreamDataLocation> extra_locations;

  // Optional locale override (for example, "tr_TR" or "C") passed to
  // as_pool_set_locale() when non-empty.
  std::string locale;
};

// Creates a lazily initialized, runtime-loaded libappstream backend using
// GModule. Adds zero compile-time or link-time dependency on libappstream and
// zero overhead to plugin startup or legacy listApps().
std::unique_ptr<AppStreamBackend> create_libappstream_backend(
    LibAppStreamConfig config = LibAppStreamConfig());

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LIBAPPSTREAM_BACKEND_H_
