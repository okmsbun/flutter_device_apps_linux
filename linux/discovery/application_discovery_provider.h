#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_DISCOVERY_PROVIDER_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_DISCOVERY_PROVIDER_H_

#include <string>
#include <vector>

#include "discovered_application.h"

namespace flutter_device_apps_linux {

// Result of a single provider's discovery pass.
// Establishes an explicit boundary for provider failure isolation.
struct DiscoveryResult {
  std::vector<DiscoveredApplication> applications;
  bool ok = true;
  std::string error_message;
};

// Internal abstraction representing a single application discovery source.
class ApplicationDiscoveryProvider {
 public:
  virtual ~ApplicationDiscoveryProvider() = default;

  // Stable identifier for this discovery source (for example, "gio").
  virtual const char* id() const = 0;

  // Returns whether this discovery source is available in the current session.
  virtual bool is_available() const = 0;

  // Discovers applications from this source.
  virtual DiscoveryResult discover() = 0;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_DISCOVERY_PROVIDER_H_
