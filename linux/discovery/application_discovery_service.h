#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_DISCOVERY_SERVICE_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_DISCOVERY_SERVICE_H_

#include <cstddef>
#include <memory>
#include <vector>

#include "application_discovery_provider.h"

namespace flutter_device_apps_linux {

// Internal coordinator that manages discovery providers and collects results.
class ApplicationDiscoveryService {
 public:
  ApplicationDiscoveryService() = default;
  ~ApplicationDiscoveryService() = default;

  ApplicationDiscoveryService(const ApplicationDiscoveryService&) = delete;
  ApplicationDiscoveryService& operator=(const ApplicationDiscoveryService&) =
      delete;
  ApplicationDiscoveryService(ApplicationDiscoveryService&&) = default;
  ApplicationDiscoveryService& operator=(ApplicationDiscoveryService&&) =
      default;

  // Registers a discovery provider in invocation order. Null pointers are
  // ignored.
  void register_provider(std::unique_ptr<ApplicationDiscoveryProvider> provider);

  // Returns the number of registered discovery providers.
  size_t provider_count() const;

  // Invokes available registered providers and collects their results.
  std::vector<DiscoveredApplication> discover_applications();

 private:
  std::vector<std::unique_ptr<ApplicationDiscoveryProvider>> providers_;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_DISCOVERY_SERVICE_H_
