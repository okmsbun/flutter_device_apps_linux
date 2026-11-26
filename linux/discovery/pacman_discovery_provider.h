#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_PACMAN_DISCOVERY_PROVIDER_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_PACMAN_DISCOVERY_PROVIDER_H_

#include <memory>
#include <string>
#include <vector>

#include "package_discovery_provider.h"

namespace flutter_device_apps_linux {

// Classifies errors reported by a Pacman/libalpm backend.
enum class PacmanQueryErrorKind {
  kNone,
  kLibraryUnavailable,
  kUnsupportedAbi,
  kDatabaseUnavailable,
  kPermissionDenied,
  kQueryFailed,
};

// Resolved Pacman root and local database paths following pacman.conf(5).
struct PacmanResolvedPaths {
  std::string root_dir = "/";
  std::string db_path = "/var/lib/pacman/";
};

// Authoritative metadata for a single installed Pacman local DB package.
struct PacmanInstalledPackageRecord {
  std::string name;
  OptionalString version;
  OptionalString architecture;
  OptionalString description;
  OptionalString homepage;
  std::vector<std::string> licenses;
  std::vector<std::string> groups;
  OptionalString packager;
  PackageInstallReason install_reason = PackageInstallReason::kUnknown;
};

// Raw result returned by a PacmanBackend implementation.
struct PacmanBackendQueryResult {
  bool ok = true;
  PacmanQueryErrorKind error_kind = PacmanQueryErrorKind::kNone;
  std::string error_message;
  std::vector<PacmanInstalledPackageRecord> packages;
};

// Exact file ownership record from the libalpm local package file list.
struct PacmanFileOwnerRecord {
  std::string path;  // Absolute target path that matched.
  std::string package_name;
  OptionalString architecture;
};

struct PacmanFileOwnershipResult {
  bool ok = true;
  PacmanQueryErrorKind error_kind = PacmanQueryErrorKind::kNone;
  std::string error_message;
  std::vector<PacmanFileOwnerRecord> owners;
};

// Testable backend seam decoupling Pacman discovery logic from runtime libalpm
// dynamic loading.
class PacmanBackend {
 public:
  virtual ~PacmanBackend() = default;

  // Returns whether libalpm with a verified ABI is available at runtime.
  virtual bool is_available() const = 0;

  // Diagnostic explaining why is_available() is false, if applicable.
  virtual std::string availability_error() const { return ""; }

  // Human-readable identifier of the resolved SONAME/ABI.
  virtual std::string abi_description() const { return "unavailable"; }

  // Reads the installed Pacman local package database in read-only mode via
  // libalpm without starting transactions, modifying db.lck, or touching sync
  // repositories.
  virtual PacmanBackendQueryResult list_installed_packages() = 0;

  // Single-pass exact file ownership lookup for a fixed batch of absolute
  // target paths via the local package file lists (alpm_pkg_get_files). The
  // default implementation reports the capability as unsupported.
  virtual PacmanFileOwnershipResult find_file_owners(
      const std::vector<std::string>& /*absolute_paths*/) {
    PacmanFileOwnershipResult result;
    result.ok = false;
    result.error_kind = PacmanQueryErrorKind::kUnsupportedAbi;
    result.error_message = "file ownership lookup is not supported";
    return result;
  }
};

// Parses RootDir and DBPath from the [options] section of pacman.conf(5) using
// bounded, read-only file access. Falls back to standard defaults ("/" and
// "/var/lib/pacman/") when the file is absent or unreadable. Explicit overrides
// take precedence over pacman.conf values.
PacmanResolvedPaths resolve_pacman_db_paths(
    const std::string& pacman_conf_path = "/etc/pacman.conf",
    const std::string& root_dir_override = "",
    const std::string& db_path_override = "");

// Discovers installed Pacman local packages via an optional runtime-loaded
// libalpm backend without invoking external binaries or parsing DB files
// directly.
class PacmanDiscoveryProvider final : public PackageDiscoveryProvider {
 public:
  explicit PacmanDiscoveryProvider(std::unique_ptr<PacmanBackend> backend);
  ~PacmanDiscoveryProvider() override = default;

  const char* id() const override;
  bool is_available() const override;
  std::string abi_description() const;
  PackageDiscoveryResult discover_packages() override;

 private:
  std::unique_ptr<PacmanBackend> backend_;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_PACMAN_DISCOVERY_PROVIDER_H_
