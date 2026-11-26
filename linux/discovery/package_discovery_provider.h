#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_PACKAGE_DISCOVERY_PROVIDER_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_PACKAGE_DISCOVERY_PROVIDER_H_

#include <string>
#include <vector>

#include "installed_package.h"

namespace flutter_device_apps_linux {

// Result of a single system package provider's inventory pass.
struct PackageDiscoveryResult {
  std::vector<InstalledPackageRecord> packages;
  bool ok = true;
  std::string error_message;
};

// Internal abstraction representing a system package inventory source
// (for example, DPKG, RPM, Pacman, or Nix).
class PackageDiscoveryProvider {
 public:
  virtual ~PackageDiscoveryProvider() = default;

  // Stable identifier for this package discovery source (for example, "dpkg").
  virtual const char* id() const = 0;

  // Returns whether this package database is available on the host system.
  virtual bool is_available() const = 0;

  // Reads the local installed package inventory without elevating privileges
  // or invoking external package-manager binaries.
  virtual PackageDiscoveryResult discover_packages() = 0;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_PACKAGE_DISCOVERY_PROVIDER_H_
