#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LIBALPM_BACKEND_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LIBALPM_BACKEND_H_

#include <memory>
#include <string>
#include <vector>

#include "pacman_discovery_provider.h"

namespace flutter_device_apps_linux {

struct LibAlpmConfig {
  // Ordered SONAME candidates to probe via GModule.
  // All listed SONAMEs share identical read-only local DB query signatures,
  // alpm_list_t layout, and borrowed-pointer ownership contracts:
  // - libalpm.so.16: Pacman 7.1+ (Arch Linux current, Ubuntu 26.04+)
  // - libalpm.so.15: Pacman 7.0  (Debian 13, Ubuntu 25.04/25.10)
  // - libalpm.so.14: Pacman 6.1
  // - libalpm.so.13: Pacman 6.0  (Ubuntu 24.04 LTS, Debian 12)
  std::vector<std::string> library_candidates = {
      "libalpm.so.16",
      "libalpm.so.15",
      "libalpm.so.14",
      "libalpm.so.13",
  };

  // Path to pacman.conf(5) consulted for [options] RootDir / DBPath when
  // root_dir / db_path overrides below are empty.
  std::string pacman_conf_path = "/etc/pacman.conf";

  // Optional explicit overrides (useful for isolated tests or custom roots).
  // When empty, values are resolved from pacman_conf_path or standard defaults
  // ("/" and "/var/lib/pacman/").
  std::string root_dir;
  std::string db_path;
};

// Creates a lazily initialized, runtime-loaded libalpm backend using GModule.
// Adds zero compile-time or link-time dependency on libalpm and zero overhead
// to plugin startup or legacy GIO-only listApps().
//
// Read-only transaction safety:
// Uses only alpm_initialize, alpm_get_localdb, alpm_db_get_pkgcache, package
// metadata getters, and alpm_release. Never calls alpm_trans_init, never
// registers sync repositories, never writes to the database, and never creates
// or removes db.lck.
std::unique_ptr<PacmanBackend> create_libalpm_backend(
    LibAlpmConfig config = LibAlpmConfig());

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LIBALPM_BACKEND_H_
