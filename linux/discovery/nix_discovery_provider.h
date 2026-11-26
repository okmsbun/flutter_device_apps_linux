#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_NIX_DISCOVERY_PROVIDER_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_NIX_DISCOVERY_PROVIDER_H_

#include <cstddef>
#include <string>
#include <vector>

#include "package_discovery_provider.h"

namespace flutter_device_apps_linux {

// Describes a local Nix profile directory to inspect.
struct NixProfileSpec {
  // Logical path to the profile directory or profile symlink (for example,
  // "~/.local/state/nix/profile", "~/.nix-profile", or
  // "/nix/var/nix/profiles/default").
  std::string path;

  // Installation scope associated with this profile.
  InstallationScope scope = InstallationScope::kOther;

  // Stable logical profile identifier used for deterministic package
  // identities across generation updates (does NOT embed store hashes).
  // When empty, derived deterministically from normalized `path`.
  std::string profile_id;
};

struct NixDiscoveryConfig {
  // When true (default), discovers standard documented Nix profile locations:
  //   1. $XDG_STATE_HOME/nix/profile (or $HOME/.local/state/nix/profile)
  //   2. $HOME/.nix-profile
  //   3. <system_profiles_root>/per-user/$USER/profile
  //   4. <system_profiles_root>/default
  // Set false in isolated unit tests that supply only explicit_profiles.
  bool include_standard_profiles = true;

  // Explicitly configured local Nix profile directories to inspect in addition
  // to (or instead of) standard profile locations.
  std::vector<NixProfileSpec> explicit_profiles;

  // Optional overrides for standard environment/system path resolution so
  // unit tests can exercise standard user/XDG/system discovery deterministically
  // without mutating process-global environment variables.
  OptionalString home_dir_override;
  OptionalString xdg_state_home_override;
  OptionalString user_name_override;
  std::string system_profiles_root = "/nix/var/nix/profiles";

  // Hard upper bound on bytes read from a single manifest.json file.
  size_t max_manifest_bytes = 16 * 1024 * 1024;
};

// Discovers active installed Nix packages by reading documented local Nix
// profile directories and their manifest.json files (schema versions 1, 2, 3).
//
// Strictly read-only and local:
// - Never recursively scans /nix/store or historical profile generations.
// - Never evaluates Nix expressions or parses arbitrary manifest.nix code
//   (explicitly reports legacy manifest.nix as unsupported).
// - Never executes nix/nix-env CLI binaries or performs network requests.
// - Preserves raw packages strictly in InstalledPackageRecord with
//   classification = ApplicationClassification::kUnknown.
class NixDiscoveryProvider final : public PackageDiscoveryProvider {
 public:
  explicit NixDiscoveryProvider(
      NixDiscoveryConfig config = NixDiscoveryConfig());
  ~NixDiscoveryProvider() override = default;

  const char* id() const override;
  bool is_available() const override;
  PackageDiscoveryResult discover_packages() override;

 private:
  std::vector<NixProfileSpec> resolve_candidate_profiles() const;

  NixDiscoveryConfig config_;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_NIX_DISCOVERY_PROVIDER_H_
