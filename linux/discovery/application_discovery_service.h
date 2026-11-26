#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_DISCOVERY_SERVICE_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_DISCOVERY_SERVICE_H_

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "application_discovery_provider.h"
#include "appstream_metadata_provider.h"
#include "package_discovery_provider.h"

namespace flutter_device_apps_linux {

// Controls which registered providers execute during a discovery pass.
// Empty provider_ids selects all registered providers; non-empty selects only
// providers whose id() matches an entry, without touching unselected providers.
struct DiscoveryRequestOptions {
  std::vector<std::string> provider_ids;

  static DiscoveryRequestOptions all_providers() {
    return DiscoveryRequestOptions{};
  }

  static DiscoveryRequestOptions only_providers(
      std::vector<std::string> ids) {
    DiscoveryRequestOptions options;
    options.provider_ids = std::move(ids);
    return options;
  }
};

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

// Aggregated result of a discovery pass across all selected providers.
// Confirmed application records and raw system-package inventory records remain
// strictly separate collections.
struct DiscoveryPassResult {
  std::vector<DiscoveredApplication> applications;
  std::vector<InstalledPackageRecord> packages;
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

  // Invokes selected available registered providers in registration order and
  // returns both discovered applications and per-provider execution summaries.
  // Unselected providers do not perform availability checks or discovery.
  DiscoveryPassResult discover_with_report(
      const DiscoveryRequestOptions& options =
          DiscoveryRequestOptions::all_providers());

  // Registers an installed-package inventory provider (for example, DPKG).
  // Package records remain strictly separate from confirmed application records.
  void register_package_provider(
      std::unique_ptr<PackageDiscoveryProvider> provider);

  // Returns the number of registered package inventory providers.
  size_t package_provider_count() const;

  // Convenience entry point returning only collected applications.
  std::vector<DiscoveredApplication> discover_applications(
      const DiscoveryRequestOptions& options =
          DiscoveryRequestOptions::all_providers());

  // Convenience entry point returning only collected system package records.
  std::vector<InstalledPackageRecord> discover_packages(
      const DiscoveryRequestOptions& options =
          DiscoveryRequestOptions::all_providers());

  // Registers an optional AppStream metadata provider kept strictly separate
  // from installed-application discovery providers. AppStream records never
  // enter discover_applications() / discover_with_report().
  void set_metadata_provider(
      std::unique_ptr<AppStreamMetadataProvider> provider);

  // Returns whether an optional AppStream metadata provider is configured.
  bool has_metadata_provider() const;

  // Loads local AppStream metadata candidates independently of installed
  // application discovery.
  AppStreamMetadataResult load_appstream_metadata();

 private:
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<ApplicationDiscoveryProvider>> providers_;
  std::vector<std::unique_ptr<PackageDiscoveryProvider>> package_providers_;
  std::unique_ptr<AppStreamMetadataProvider> metadata_provider_;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_DISCOVERY_SERVICE_H_
