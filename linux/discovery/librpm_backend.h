#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LIBRPM_BACKEND_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LIBRPM_BACKEND_H_

#include <memory>
#include <string>
#include <vector>

#include "rpm_discovery_provider.h"

namespace flutter_device_apps_linux {

struct LibRpmConfig {
  // Ordered SONAME candidates to probe via GModule.
  // - librpm.so.10: RPM 4.19 / 4.20 / 6.0 (Fedora 40+, RHEL 10, Ubuntu 24.04+)
  // - librpm.so.9:  RPM 4.16 - 4.18 (Fedora 35-39, RHEL 9, Debian 12, Ubuntu 22.04)
  // - librpm.so.8:  RPM 4.14 - 4.15 (RHEL 8, Debian 11)
  std::vector<std::string> library_candidates = {
      "librpm.so.10",
      "librpm.so.9",
      "librpm.so.8",
  };

  // Optional per-transaction root directory passed to rpmtsSetRootDir() for
  // isolated integration tests without mutating process-global RPM macros.
  // Empty uses RPM's configured system database location.
  std::string root_dir;
};

// Creates a lazily initialized, runtime-loaded librpm backend using GModule.
// Adds zero compile-time or link-time dependency on librpm and zero overhead
// to plugin startup or legacy GIO-only listApps().
std::unique_ptr<RpmBackend> create_librpm_backend(
    LibRpmConfig config = LibRpmConfig());

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LIBRPM_BACKEND_H_
