#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_INSTALLED_PACKAGE_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_INSTALLED_PACKAGE_H_

#include <string>

#include "discovered_application.h"

namespace flutter_device_apps_linux {

// Actual installation state of a system package, kept distinct from desired
// selection state (such as install/hold/deinstall/purge).
enum class PackageInstallationState {
  kUnknown,
  kInstalled,
  kTriggersAwaited,
  kTriggersPending,
};

// Desired selection state recorded by the package manager.
enum class PackageSelectionState {
  kUnknown,
  kInstall,
  kHold,
  kDeinstall,
  kPurge,
};

// Internal installed system-package inventory record.
// Kept strictly separate from confirmed DiscoveredApplication records so raw
// distribution packages (libraries, daemons, base utilities) are never treated
// as confirmed user applications without authoritative evidence.
struct InstalledPackageRecord {
  // Deterministic provider-scoped package identity, stable across version
  // upgrades (for example, "dpkg:ripgrep:system:amd64").
  std::string package_identity_id;

  // Source provenance (provider ID, native package name, installation source,
  // and installation scope).
  SourceProvenance provenance;

  // Authoritative package fields.
  std::string package_name;
  OptionalString architecture;
  OptionalString version;
  OptionalString summary;
  OptionalString description;
  OptionalString section;
  OptionalString priority;
  OptionalString homepage;
  OptionalString maintainer;

  // Actual installation state vs desired selection state.
  PackageInstallationState installation_state =
      PackageInstallationState::kUnknown;
  PackageSelectionState selection_state = PackageSelectionState::kUnknown;

  // Classification remains kUnknown unless authoritative evidence (such as
  // future AppStream or desktop integration matching) establishes it.
  ApplicationClassification classification =
      ApplicationClassification::kUnknown;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_INSTALLED_PACKAGE_H_
