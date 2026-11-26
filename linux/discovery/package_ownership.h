#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_PACKAGE_OWNERSHIP_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_PACKAGE_OWNERSHIP_H_

#include <string>
#include <unordered_map>
#include <vector>

#include "discovered_application.h"
#include "installed_package.h"

namespace flutter_device_apps_linux {

// Authoritative file-to-package ownership evidence returned by a documented
// package database lookup (for example, DPKG info/*.list + diversions, RPM
// RPMDBI_BASENAMES + rpmfi full-path check, libalpm alpm_pkg_get_files, or
// verified Nix store path ownership).
struct PackageOwnershipEvidence {
  std::string package_provider_id;  // "dpkg", "rpm", "pacman", "nix"
  OptionalString package_identity_id;
  OptionalString installation_identity_id;
  std::string package_name;
  OptionalString architecture;
  InstallationScope scope = InstallationScope::kSystem;
  OptionalString profile_path;

  // True when a local diversion (such as dpkg-divert --local) or conflicting
  // diversion prevents attributing the queried path unambiguously to a single
  // owning package.
  bool diverted_ambiguous = false;
  std::string diversion_detail;
};

// Optional seam for querying documented package ownership databases for a
// specific file path (such as a .desktop file). Implementations must never use
// filesystem heuristics, recursive scanning, or shell commands.
class PackageOwnershipLookup {
 public:
  virtual ~PackageOwnershipLookup() = default;

  // Returns all authoritative package owners for `file_path`. Returns empty if
  // unknown, or multiple entries if ownership is shared/ambiguous.
  virtual std::vector<PackageOwnershipEvidence> lookup_file_owners(
      const std::string& file_path) const = 0;
};

// Result of building a single-pass, bounded file-ownership index for a fixed
// batch of target paths from one package provider.
struct PackageOwnershipIndexResult {
  bool ok = true;
  std::string error_message;
  std::unordered_map<std::string, std::vector<PackageOwnershipEvidence>>
      owners_by_path;
};

// Builds a bounded ownership index ONCE per discovery pass for a fixed batch
// of target paths (such as .desktop files discovered by GIO). Indexers read
// only authoritative local package databases; they never scan the filesystem
// recursively, execute package-manager CLIs, or touch the network.
class PackageOwnershipIndexer {
 public:
  virtual ~PackageOwnershipIndexer() = default;

  // Provider identifier whose installed packages this indexer can resolve
  // ("dpkg", "rpm", "pacman", "nix").
  virtual const char* id() const = 0;

  virtual PackageOwnershipIndexResult build_index(
      const std::vector<std::string>& target_paths,
      const std::vector<InstalledPackageRecord>& packages) = 0;
};

// Normalizes an absolute filesystem path by collapsing repeated '/' separators
// and stripping trailing '/' (except root "/").
std::string normalize_ownership_path(const std::string& path);

// Resolves symlinks via POSIX realpath() when the path exists on disk. Returns
// empty string when the path does not exist or cannot be resolved.
std::string resolve_canonical_file_path(const std::string& path);

// Single-pass, read-only composite ownership index aggregating bounded results
// from DPKG, RPM, Pacman, and other providers without re-querying databases
// per desktop entry.
class CompositePackageOwnershipLookup final : public PackageOwnershipLookup {
 public:
  void merge(const PackageOwnershipIndexResult& index_result);

  std::vector<PackageOwnershipEvidence> lookup_file_owners(
      const std::string& file_path) const override;

  bool empty() const { return owners_by_path_.empty(); }

 private:
  std::unordered_map<std::string, std::vector<PackageOwnershipEvidence>>
      owners_by_path_;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_PACKAGE_OWNERSHIP_H_
