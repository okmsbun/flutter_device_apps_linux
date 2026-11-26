#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_SNAP_DISCOVERY_PROVIDER_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_SNAP_DISCOVERY_PROVIDER_H_

#include <memory>
#include <string>
#include <vector>

#include "application_discovery_provider.h"

namespace flutter_device_apps_linux {

// Classifies failures from querying the local snapd daemon.
enum class SnapQueryErrorKind {
  kNone,
  kDaemonUnavailable,
  kPermissionDenied,
  kTimeout,
  kProtocolError,
  kQueryFailed,
};

// Authoritative metadata for a single active installed snap returned by
// snapd's GET /v2/snaps?select=enabled endpoint.
struct SnapInstalledRecord {
  // Authoritative instance identity (for example, "firefox" or "firefox_beta").
  std::string instance_name;
  // Base snap package name (for example, "firefox").
  std::string package_name;
  // Optional parallel installation key (for example, "beta").
  OptionalString instance_key;
  // Snap package type (for example, "app", "base", "core", "kernel", "gadget",
  // "snapd"). Only "app" is included as a discovered application.
  std::string type;
  // Reported status (for example, "active").
  OptionalString status;
  // Optional human-readable display title.
  OptionalString title;
  OptionalString version;
  OptionalString revision;
  OptionalString channel;
  OptionalString summary;
  OptionalString description;
  OptionalString license;
};

// Result of querying snapd for active installed snaps.
struct SnapBackendQueryResult {
  bool ok = true;
  SnapQueryErrorKind error_kind = SnapQueryErrorKind::kNone;
  std::string error_message;
  std::vector<SnapInstalledRecord> snaps;
};

// Testable local snapd transport/query abstraction.
class SnapBackend {
 public:
  virtual ~SnapBackend() = default;

  // Returns whether the local snapd socket/backend appears available.
  virtual bool is_available() const = 0;

  // Queries local snapd GET /v2/snaps?select=enabled (read-only, no network).
  virtual SnapBackendQueryResult list_enabled_snaps() = 0;
};

// Discovers active installed application-type snaps via the local snapd REST
// API, independently of desktop entries.
//
// Scope note:
// Queries active/enabled snaps only (GET /v2/snaps?select=enabled). Disabled
// snap installations and historical inactive revisions are intentionally
// outside this initial discovery scope.
class SnapDiscoveryProvider final : public ApplicationDiscoveryProvider {
 public:
  explicit SnapDiscoveryProvider(std::unique_ptr<SnapBackend> backend);
  ~SnapDiscoveryProvider() override = default;

  const char* id() const override;
  bool is_available() const override;
  DiscoveryResult discover() override;

 private:
  std::unique_ptr<SnapBackend> backend_;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_SNAP_DISCOVERY_PROVIDER_H_
