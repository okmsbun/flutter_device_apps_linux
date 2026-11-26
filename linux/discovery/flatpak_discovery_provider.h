#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_FLATPAK_DISCOVERY_PROVIDER_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_FLATPAK_DISCOVERY_PROVIDER_H_

#include <memory>
#include <string>
#include <vector>

#include "application_discovery_provider.h"

namespace flutter_device_apps_linux {

// Mirrors FlatpakRefKind values from libflatpak's flatpak-ref.h.
constexpr int kFlatpakRefKindApp = 0;
constexpr int kFlatpakRefKindRuntime = 1;

// Authoritative metadata for a single installed Flatpak reference returned by
// the libflatpak backend.
struct FlatpakInstalledAppRecord {
  std::string app_id;
  std::string arch;
  std::string branch;
  int ref_kind = kFlatpakRefKindApp;
  OptionalString name;
  OptionalString version;
  OptionalString summary;
};

// Query result for a single Flatpak installation (user, default system, or
// named system installation).
struct FlatpakInstallationQueryResult {
  std::string installation_id;
  bool is_user = false;
  bool ok = true;
  std::string error_message;
  std::vector<FlatpakInstalledAppRecord> apps;
};

// Aggregated result from querying all accessible Flatpak installations.
struct FlatpakBackendQueryResult {
  bool ok = true;
  std::string error_message;
  std::vector<FlatpakInstallationQueryResult> installations;
};

// Internal backend seam decoupling Flatpak discovery logic from runtime
// libflatpak dynamic loading so discovery behavior is deterministically
// testable without libflatpak installed.
class FlatpakBackend {
 public:
  virtual ~FlatpakBackend() = default;

  // Returns whether libflatpak and required symbols are available at runtime.
  virtual bool is_available() const = 0;

  // Performs read-only local discovery across user and configured system
  // Flatpak installations.
  virtual FlatpakBackendQueryResult list_installed_apps() = 0;
};

// Builds a collision-free installation-specific identity from immutable
// Flatpak installation coordinates (installation_id, branch, arch).
std::string build_flatpak_installation_id(const std::string& installation_name,
                                          const std::string& branch,
                                          const std::string& arch);

// Discovers installed Flatpak applications through an optional libflatpak
// backend, independently of desktop entries.
class FlatpakDiscoveryProvider final : public ApplicationDiscoveryProvider {
 public:
  explicit FlatpakDiscoveryProvider(std::unique_ptr<FlatpakBackend> backend);
  ~FlatpakDiscoveryProvider() override = default;

  const char* id() const override;
  bool is_available() const override;
  DiscoveryResult discover() override;

 private:
  std::unique_ptr<FlatpakBackend> backend_;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_FLATPAK_DISCOVERY_PROVIDER_H_
