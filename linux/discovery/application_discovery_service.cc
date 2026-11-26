#include "application_discovery_service.h"

#include <utility>

namespace flutter_device_apps_linux {

void ApplicationDiscoveryService::register_provider(
    std::unique_ptr<ApplicationDiscoveryProvider> provider) {
  if (provider == nullptr) return;
  providers_.push_back(std::move(provider));
}

size_t ApplicationDiscoveryService::provider_count() const {
  return providers_.size();
}

std::vector<DiscoveredApplication>
ApplicationDiscoveryService::discover_applications() {
  std::vector<DiscoveredApplication> applications;
  for (const auto& provider : providers_) {
    if (!provider->is_available()) continue;

    DiscoveryResult result = provider->discover();
    if (!result.ok) {
      // Failure isolation boundary: ignore failed provider output without
      // interrupting other registered providers.
      continue;
    }

    applications.reserve(applications.size() + result.applications.size());
    for (auto& app : result.applications) {
      applications.push_back(std::move(app));
    }
  }
  return applications;
}

}  // namespace flutter_device_apps_linux
