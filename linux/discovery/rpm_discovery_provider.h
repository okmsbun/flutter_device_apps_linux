#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_RPM_DISCOVERY_PROVIDER_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_RPM_DISCOVERY_PROVIDER_H_

#include <memory>
#include <string>
#include <vector>

#include "package_discovery_provider.h"

namespace flutter_device_apps_linux {

// Classifies errors reported by an RPM backend.
enum class RpmQueryErrorKind {
  kNone,
  kLibraryUnavailable,
  kUnsupportedAbi,
  kDatabaseUnavailable,
  kPermissionDenied,
  kQueryFailed,
};

// Authoritative metadata for a single installed RPM package header.
struct RpmInstalledPackageRecord {
  std::string name;
  OptionalString epoch;
  OptionalString version;
  OptionalString release;
  OptionalString architecture;
  OptionalString summary;
  OptionalString description;
  OptionalString license;
  OptionalString homepage;
  OptionalString vendor;
  OptionalString package_group;
};

// Raw result returned by an RpmBackend implementation.
struct RpmBackendQueryResult {
  bool ok = true;
  RpmQueryErrorKind error_kind = RpmQueryErrorKind::kNone;
  std::string error_message;
  std::vector<RpmInstalledPackageRecord> packages;
};

// Exact file ownership record verified through rpmfi full-path comparison.
struct RpmFileOwnerRecord {
  std::string path;
  RpmInstalledPackageRecord package;
};

struct RpmFileOwnershipResult {
  bool ok = true;
  RpmQueryErrorKind error_kind = RpmQueryErrorKind::kNone;
  std::string error_message;
  std::vector<RpmFileOwnerRecord> owners;
};

// Testable backend seam decoupling RPM discovery logic from runtime librpm
// dynamic loading.
class RpmBackend {
 public:
  virtual ~RpmBackend() = default;

  // Returns whether librpm with a verified ABI is available at runtime.
  virtual bool is_available() const = 0;

  // Diagnostic explaining why is_available() is false, if applicable.
  virtual std::string availability_error() const { return ""; }

  // Human-readable identifier of the resolved SONAME/ABI.
  virtual std::string abi_description() const { return "unavailable"; }

  // Reads the installed RPM database in read-only mode via librpm APIs.
  virtual RpmBackendQueryResult list_installed_packages() = 0;

  // Exact full-path file ownership lookup for a fixed batch of absolute
  // target paths (librpm basename index + rpmfi verification). The default
  // implementation reports the capability as unsupported.
  virtual RpmFileOwnershipResult find_file_owners(
      const std::vector<std::string>& /*absolute_paths*/) {
    RpmFileOwnershipResult result;
    result.ok = false;
    result.error_kind = RpmQueryErrorKind::kUnsupportedAbi;
    result.error_message = "file ownership lookup is not supported";
    return result;
  }
};

// Builds a collision-free version-specific installation identity for an RPM
// package from (name, arch, epoch, version, release), keeping multiple
// simultaneously installed versions distinct.
std::string build_rpm_installation_identity_id(
    const std::string& package_name, const OptionalString& architecture,
    const OptionalString& epoch, const OptionalString& version,
    const OptionalString& release);

// Discovers installed RPM packages via an optional runtime-loaded librpm
// backend without invoking external binaries or parsing DB files directly.
class RpmDiscoveryProvider final : public PackageDiscoveryProvider {
 public:
  explicit RpmDiscoveryProvider(std::unique_ptr<RpmBackend> backend);
  ~RpmDiscoveryProvider() override = default;

  const char* id() const override;
  bool is_available() const override;
  std::string abi_description() const;
  PackageDiscoveryResult discover_packages() override;

 private:
  std::unique_ptr<RpmBackend> backend_;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_RPM_DISCOVERY_PROVIDER_H_
