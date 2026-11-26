#include "application_discovery_service.h"

#include <utility>

namespace flutter_device_apps_linux {

void ApplicationDiscoveryService::register_provider(
    std::unique_ptr<ApplicationDiscoveryProvider> provider) {
  if (provider == nullptr) return;
  std::lock_guard<std::mutex> lock(mutex_);
  providers_.push_back(std::move(provider));
}

size_t ApplicationDiscoveryService::provider_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return providers_.size();
}

DiscoveryPassResult ApplicationDiscoveryService::discover_with_report() {
  std::lock_guard<std::mutex> lock(mutex_);
  DiscoveryPassResult pass_result;
  pass_result.provider_summaries.reserve(providers_.size());

  for (const auto& provider : providers_) {
    ProviderExecutionSummary summary;
    const char* raw_id = provider->id();
    summary.provider_id = raw_id == nullptr ? "" : raw_id;

    if (!provider->is_available()) {
      summary.status = ProviderExecutionStatus::kUnavailable;
      pass_result.provider_summaries.push_back(std::move(summary));
      continue;
    }

    DiscoveryResult result = provider->discover();
    if (!result.ok) {
      // Failure isolation boundary: discard partial payload from failed
      // providers without interrupting remaining providers.
      summary.status = ProviderExecutionStatus::kFailed;
      summary.error_message = std::move(result.error_message);
      pass_result.provider_summaries.push_back(std::move(summary));
      continue;
    }

    summary.status = ProviderExecutionStatus::kSucceeded;
    summary.discovered_count = result.applications.size();
    pass_result.provider_summaries.push_back(std::move(summary));

    pass_result.applications.reserve(pass_result.applications.size() +
                                     result.applications.size());
    for (auto& app : result.applications) {
      pass_result.applications.push_back(std::move(app));
    }
  }

  return pass_result;
}

std::vector<DiscoveredApplication>
ApplicationDiscoveryService::discover_applications() {
  return discover_with_report().applications;
}

}  // namespace flutter_device_apps_linux
