#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_DISCOVERY_SERVICE_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_DISCOVERY_SERVICE_H_

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "application_discovery_provider.h"

namespace flutter_device_apps_linux {

// Per-provider outcome recorded during a discovery pass.
enum class ProviderExecutionStatus {
  kSucceeded,
  kPartiallyFailed,
  kUnavailable,
  kFailed,
};

// Summary of a single provider's participation in a discovery pass.
struct ProviderExecutionSummary {
  std::string provider_id;
  ProviderExecutionStatus status = ProviderExecutionStatus::kSucceeded;
  size_t discovered_count = 0;
  std::string error_message;
};

// Aggregated result of a discovery pass across all registered providers.
struct DiscoveryPassResult {
  std::vector<DiscoveredApplication> applications;
  std::vector<ProviderExecutionSummary> provider_summaries;
};

// Internal coordinator that manages discovery providers and collects results.
//
// Thread safety:
// - Provider registration and discovery passes are internally serialized via a
//   mutex so providers are never accessed concurrently and registration cannot
//   race with active discovery.
// - Intended to be executed off the platform thread through
//   AsyncDiscoveryRunner once potentially blocking providers are present.
class ApplicationDiscoveryService {
 public:
  ApplicationDiscoveryService() = default;
  ~ApplicationDiscoveryService() = default;

  ApplicationDiscoveryService(const ApplicationDiscoveryService&) = delete;
  ApplicationDiscoveryService& operator=(const ApplicationDiscoveryService&) =
      delete;
  ApplicationDiscoveryService(ApplicationDiscoveryService&&) = delete;
  ApplicationDiscoveryService& operator=(ApplicationDiscoveryService&&) =
      delete;

  // Registers a discovery provider in invocation order. Null pointers are
  // ignored.
  void register_provider(std::unique_ptr<ApplicationDiscoveryProvider> provider);

  // Returns the number of registered discovery providers.
  size_t provider_count() const;

  // Invokes available registered providers in registration order and returns
  // both discovered applications and per-provider execution summaries.
  DiscoveryPassResult discover_with_report();

  // Convenience entry point returning only collected applications.
  std::vector<DiscoveredApplication> discover_applications();

 private:
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<ApplicationDiscoveryProvider>> providers_;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_DISCOVERY_SERVICE_H_
