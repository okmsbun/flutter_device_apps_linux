#include "application_discovery_service.h"

#include <algorithm>
#include <utility>

namespace flutter_device_apps_linux {
namespace {

bool is_provider_selected(const DiscoveryRequestOptions& options,
                          const std::string& provider_id) {
  if (options.provider_ids.empty()) return true;
  return std::find(options.provider_ids.begin(), options.provider_ids.end(),
                   provider_id) != options.provider_ids.end();
}

}  // namespace

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

DiscoveryPassResult ApplicationDiscoveryService::discover_with_report(
    const DiscoveryRequestOptions& options) {
  std::lock_guard<std::mutex> lock(mutex_);
  DiscoveryPassResult pass_result;
  pass_result.provider_summaries.reserve(providers_.size());

  for (const auto& provider : providers_) {
    const char* raw_id = provider->id();
    const std::string provider_id = raw_id == nullptr ? "" : raw_id;
    if (!is_provider_selected(options, provider_id)) {
      // Unselected providers must not perform availability checks or discovery.
      continue;
    }

    ProviderExecutionSummary summary;
    summary.provider_id = provider_id;

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

    summary.status = result.error_message.empty()
                         ? ProviderExecutionStatus::kSucceeded
                         : ProviderExecutionStatus::kPartiallyFailed;
    summary.discovered_count = result.applications.size();
    summary.error_message = std::move(result.error_message);
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
ApplicationDiscoveryService::discover_applications(
    const DiscoveryRequestOptions& options) {
  return discover_with_report(options).applications;
}

void ApplicationDiscoveryService::set_metadata_provider(
    std::unique_ptr<AppStreamMetadataProvider> provider) {
  std::lock_guard<std::mutex> lock(mutex_);
  metadata_provider_ = std::move(provider);
}

bool ApplicationDiscoveryService::has_metadata_provider() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return metadata_provider_ != nullptr;
}

AppStreamMetadataResult ApplicationDiscoveryService::load_appstream_metadata() {
  std::lock_guard<std::mutex> lock(mutex_);
  AppStreamMetadataResult result;
  if (metadata_provider_ == nullptr) {
    result.ok = false;
    result.error_message = "No AppStream metadata provider registered";
    return result;
  }
  return metadata_provider_->load_metadata();
}

}  // namespace flutter_device_apps_linux
