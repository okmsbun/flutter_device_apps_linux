#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_PACKAGE_OWNERSHIP_INDEXERS_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_PACKAGE_OWNERSHIP_INDEXERS_H_

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "package_ownership.h"
#include "pacman_discovery_provider.h"
#include "rpm_discovery_provider.h"

namespace flutter_device_apps_linux {

struct DpkgOwnershipConfig {
  std::string info_dir = "/var/lib/dpkg/info";
  std::string diversions_path = "/var/lib/dpkg/diversions";
  size_t max_list_file_bytes = 64 * 1024 * 1024;
  size_t max_diversions_bytes = 4 * 1024 * 1024;
};

// DPKG: reads authoritative per-package file inventories
// (/var/lib/dpkg/info/<pkg>[:<arch>].list) in bounded streaming mode and
// matches exact normalized paths. Paths named in /var/lib/dpkg/diversions are
// flagged as ambiguous instead of being attributed.
std::unique_ptr<PackageOwnershipIndexer> create_dpkg_ownership_indexer(
    DpkgOwnershipConfig config = DpkgOwnershipConfig());

// Nix: verified store-path ownership for entries of ACTIVE profile elements
// (InstalledPackageRecord.store_paths). Never scans /nix/store.
std::unique_ptr<PackageOwnershipIndexer> create_nix_ownership_indexer();

// RPM: uses RpmBackend::find_file_owners() (librpm basename index + exact
// full-path verification through rpmfi).
std::unique_ptr<PackageOwnershipIndexer> create_rpm_ownership_indexer(
    std::unique_ptr<RpmBackend> backend);

// Pacman: uses PacmanBackend::find_file_owners() (libalpm
// alpm_pkg_get_files over the local database).
std::unique_ptr<PackageOwnershipIndexer> create_pacman_ownership_indexer(
    std::unique_ptr<PacmanBackend> backend);

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_PACKAGE_OWNERSHIP_INDEXERS_H_
