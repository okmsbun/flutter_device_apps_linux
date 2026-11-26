#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_DPKG_DISCOVERY_PROVIDER_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_DPKG_DISCOVERY_PROVIDER_H_

#include <cstddef>
#include <string>

#include "package_discovery_provider.h"

namespace flutter_device_apps_linux {

struct DpkgDiscoveryConfig {
  std::string status_file_path = "/var/lib/dpkg/status";
  size_t max_file_bytes = 64 * 1024 * 1024;
  size_t max_stanza_bytes = 1024 * 1024;
};

// Discovers installed Debian packages by reading /var/lib/dpkg/status directly
// in bounded, read-only streaming mode (deb822 format).
//
// Snapshot consistency note:
// Reads /var/lib/dpkg/status directly without acquiring dpkg locks and without
// replaying /var/lib/dpkg/updates/. While dpkg atomically replaces the status
// file on flush, concurrent package-manager transactions may momentarily expose
// transient states (such as unpacked, half-configured, or triggers-pending).
class DpkgDiscoveryProvider final : public PackageDiscoveryProvider {
 public:
  explicit DpkgDiscoveryProvider(
      DpkgDiscoveryConfig config = DpkgDiscoveryConfig());
  ~DpkgDiscoveryProvider() override = default;

  const char* id() const override;
  bool is_available() const override;
  PackageDiscoveryResult discover_packages() override;

 private:
  DpkgDiscoveryConfig config_;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_DPKG_DISCOVERY_PROVIDER_H_
