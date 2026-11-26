#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_GIO_DISCOVERY_PROVIDER_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_GIO_DISCOVERY_PROVIDER_H_

#include "application_discovery_provider.h"

namespace flutter_device_apps_linux {

// Discovers visible desktop applications through GIO's GAppInfo API.
class GioDiscoveryProvider final : public ApplicationDiscoveryProvider {
 public:
  GioDiscoveryProvider() = default;
  ~GioDiscoveryProvider() override = default;

  const char* id() const override;
  bool is_available() const override;
  DiscoveryResult discover() override;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_GIO_DISCOVERY_PROVIDER_H_
