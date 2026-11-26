// Linux-only native integration test for PacmanDiscoveryProvider and
// create_libalpm_backend() runtime loading via GModule.
//
// Modes:
//   --missing-lib
//       Verifies graceful unavailability when libalpm is absent.
//   --unsupported-abi <incomplete.so>
//       Verifies missing required libalpm symbols report unavailable cleanly.
//   --empty <root_dir> <empty_dbpath>
//       Queries an empty isolated Pacman local DB directory.
//   --fixture <root_dir> <fixture_dbpath> <pacman_conf_path>
//       Queries an isolated offline local Pacman database configured via
//       pacman.conf DBPath, verifies db.lck non-interference (both absent and
//       pre-existing active lockfile), and compares results against
//       `pacman --root <root> --dbpath <dbpath> -Q` (test-only oracle).

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "discovery/libalpm_backend.h"
#include "discovery/pacman_discovery_provider.h"

namespace {

using flutter_device_apps_linux::ApplicationClassification;
using flutter_device_apps_linux::create_libalpm_backend;
using flutter_device_apps_linux::InstallationScope;
using flutter_device_apps_linux::InstallationSource;
using flutter_device_apps_linux::LibAlpmConfig;
using flutter_device_apps_linux::PackageDiscoveryResult;
using flutter_device_apps_linux::PackageInstallationState;
using flutter_device_apps_linux::PackageInstallReason;
using flutter_device_apps_linux::PacmanDiscoveryProvider;

int g_failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

bool path_exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

std::string read_file_text(const std::string& path) {
  std::ifstream in(path.c_str(), std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

int run_missing_lib() {
  LibAlpmConfig cfg;
  cfg.library_candidates = {"libalpm-does-not-exist.so.999"};
  PacmanDiscoveryProvider provider(create_libalpm_backend(cfg));
  expect(!provider.is_available(),
         "missing libalpm must report is_available() == false");
  const PackageDiscoveryResult res = provider.discover_packages();
  expect(!res.ok && res.error_message.find("failed to load") !=
                        std::string::npos,
         "missing libalpm diagnostic must report dynamic load failure");
  if (g_failures > 0) return 1;
  std::cout << "Pacman missing-library native test passed.\n";
  return 0;
}

int run_unsupported_abi(const char* incomplete_so) {
  LibAlpmConfig cfg;
  cfg.library_candidates = {incomplete_so};
  PacmanDiscoveryProvider provider(create_libalpm_backend(cfg));
  expect(!provider.is_available(),
         "library missing required libalpm symbols must report unavailable");
  const PackageDiscoveryResult res = provider.discover_packages();
  expect(!res.ok && res.error_message.find("missing required symbols") !=
                        std::string::npos,
         "missing required symbols diagnostic must be distinguishable");
  if (g_failures > 0) return 1;
  std::cout << "Pacman unsupported-ABI native test passed.\n";
  return 0;
}

int run_empty(const char* root_dir, const char* empty_dbpath) {
  LibAlpmConfig cfg;
  cfg.root_dir = root_dir;
  cfg.db_path = empty_dbpath;
  PacmanDiscoveryProvider provider(create_libalpm_backend(cfg));
  if (!provider.is_available()) {
    const auto diag = provider.discover_packages();
    std::cerr << "FAIL: libalpm unavailable: " << diag.error_message << "\n";
    return 1;
  }
  const PackageDiscoveryResult res = provider.discover_packages();
  expect(res.ok && res.packages.empty(),
         "empty isolated Pacman local DB must succeed with 0 packages");
  expect(!path_exists(std::string(empty_dbpath) + "/db.lck"),
         "read-only Pacman discovery must never create db.lck");
  if (g_failures > 0) return 1;
  std::cout << "Pacman empty-database native integration test passed.\n";
  return 0;
}

int run_fixture(const char* root_dir, const char* fixture_dbpath,
                const char* pacman_conf_path) {
  // Exercise pacman.conf DBPath resolution (no explicit db_path override).
  LibAlpmConfig cfg;
  cfg.pacman_conf_path = pacman_conf_path;
  PacmanDiscoveryProvider provider(create_libalpm_backend(cfg));
  if (!provider.is_available()) {
    const auto diag = provider.discover_packages();
    std::cerr << "FAIL: libalpm unavailable: " << diag.error_message << "\n";
    return 1;
  }

  const std::string lock_path = std::string(fixture_dbpath) + "/db.lck";
  expect(!path_exists(lock_path),
         "fixture db.lck must be absent before initial read-only query");

  const PackageDiscoveryResult res = provider.discover_packages();
  expect(res.ok && res.error_message.empty(),
         "populated isolated Pacman fixture discovery must succeed cleanly");
  expect(!path_exists(lock_path),
         "read-only discovery must not create db.lck");

  // Simulate an active pacman transaction by creating a sentinel db.lck file.
  // Read-only discovery must still succeed without modifying or removing it.
  {
    std::ofstream lock_out(lock_path.c_str(), std::ios::binary);
    lock_out << "99999\n";
  }
  const PackageDiscoveryResult res_while_locked = provider.discover_packages();
  expect(res_while_locked.ok &&
             res_while_locked.packages.size() == res.packages.size(),
         "read-only libalpm discovery must succeed while db.lck is present");
  expect(path_exists(lock_path) && read_file_text(lock_path) == "99999\n",
         "read-only libalpm discovery must never remove or modify db.lck");
  std::remove(lock_path.c_str());

  std::set<std::string> parsed_pairs;
  bool found_explicit = false;
  bool found_depend = false;

  for (const auto& pkg : res.packages) {
    expect(pkg.provenance.provider_id == "pacman" &&
               pkg.provenance.installation_source ==
                   InstallationSource::kPacman &&
               pkg.provenance.installation_scope ==
                   InstallationScope::kSystem &&
               pkg.installation_state ==
                   PackageInstallationState::kInstalled &&
               pkg.classification == ApplicationClassification::kUnknown,
           "Pacman provenance, state, and unknown classification must match");

    const std::string ver = pkg.version.has_value() ? pkg.version.value() : "";
    parsed_pairs.insert(pkg.package_name + " " + ver);

    if (pkg.package_name == "example-app") {
      found_explicit = true;
      expect(pkg.package_identity_id == "pacman:example-app:system:x86_64",
             "stable package_identity_id must match pacman contract");
      expect(pkg.version.has_value() && pkg.version.value() == "1.2.3-1",
             "Pacman version must match");
      expect(pkg.architecture.has_value() &&
                 pkg.architecture.value() == "x86_64",
             "Pacman architecture must match");
      expect(pkg.description.has_value() &&
                 pkg.description.value() == "Example explicit Arch package",
             "Pacman description must match");
      expect(pkg.homepage.has_value() &&
                 pkg.homepage.value() == "https://example.org/app",
             "Pacman homepage/URL must match");
      expect(pkg.packager.has_value() &&
                 pkg.packager.value() ==
                     "Example Packager <packager@example.org>",
             "Pacman packager must match");
      expect(pkg.licenses.size() == 2 && pkg.licenses[0] == "MIT" &&
                 pkg.licenses[1] == "Apache-2.0",
             "Pacman multi-license list must be preserved");
      expect(pkg.groups.size() == 2 && pkg.groups[0] == "base-devel" &&
                 pkg.groups[1] == "example-tools",
             "Pacman groups list must be preserved");
      expect(pkg.install_reason == PackageInstallReason::kExplicit,
             "explicitly installed package must report kExplicit");
    } else if (pkg.package_name == "example-lib") {
      found_depend = true;
      expect(pkg.package_identity_id == "pacman:example-lib:system:any",
             "dependency package identity must encode architecture");
      expect(pkg.install_reason == PackageInstallReason::kDependency,
             "dependency package must report kDependency");
      expect(!pkg.homepage.has_value() && pkg.groups.empty(),
             "missing optional Pacman fields must remain unset/empty");
    }
  }

  expect(found_explicit && found_depend,
         "both explicit and dependency fixture packages must be discovered");

  // Compare against `pacman --root <root> --dbpath <dbpath> -Q` as test-only
  // reference oracle.
  const std::string cmd = std::string("pacman --root '") + root_dir +
                          "' --dbpath '" + fixture_dbpath + "' -Q";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (pipe == nullptr) {
    std::cerr << "FAIL: unable to execute pacman -Q reference command\n";
    return 1;
  }

  std::set<std::string> expected_pairs;
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
    std::string line(buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    if (!line.empty()) {
      expected_pairs.insert(std::move(line));
    }
  }
  const int rc = pclose(pipe);
  expect(rc == 0, "pacman -Q reference command must exit 0");
  expect(!expected_pairs.empty(), "pacman -Q must return fixture packages");
  expect(parsed_pairs == expected_pairs,
         "PacmanDiscoveryProvider output must match pacman -Q reference output");

  // --- Runtime file ownership via alpm_pkg_get_files (exact paths only) ---
  {
    LibAlpmConfig own_cfg;
    own_cfg.pacman_conf_path = pacman_conf_path;
    auto own_backend = create_libalpm_backend(own_cfg);
    const std::string owned_path =
        std::string(root_dir) + "/usr/share/applications/example-app.desktop";
    const std::string unowned_path =
        std::string(root_dir) + "/usr/share/applications/unowned.desktop";
    const auto own = own_backend->find_file_owners({owned_path, unowned_path});
    expect(own.ok, "libalpm file ownership query must succeed");
    expect(own.owners.size() == 1 && own.owners[0].path == owned_path &&
               own.owners[0].package_name == "example-app",
           "alpm_pkg_get_files must attribute only the exact owned path");
    expect(!path_exists(lock_path),
           "ownership query must not create db.lck");

    const std::string oracle_cmd = std::string("pacman --root '") + root_dir +
                                   "' --dbpath '" + fixture_dbpath +
                                   "' -Qoq '" + owned_path + "'";
    FILE* own_pipe = popen(oracle_cmd.c_str(), "r");
    std::string oracle_name;
    if (own_pipe != nullptr) {
      char name_buf[512];
      if (std::fgets(name_buf, sizeof(name_buf), own_pipe) != nullptr) {
        oracle_name = name_buf;
        while (!oracle_name.empty() &&
               (oracle_name.back() == '\n' || oracle_name.back() == '\r')) {
          oracle_name.pop_back();
        }
      }
      pclose(own_pipe);
    }
    expect(oracle_name == "example-app",
           "pacman -Qoq reference must agree with libalpm file ownership");
  }

  if (g_failures > 0) return 1;
  std::cout << "Pacman local fixture, db.lck non-interference, and pacman -Q "
               "parity test passed ("
            << parsed_pairs.size() << " packages matched, ABI="
            << provider.abi_description() << ").\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--missing-lib") == 0) {
    return run_missing_lib();
  }
  if (argc >= 3 && std::strcmp(argv[1], "--unsupported-abi") == 0) {
    return run_unsupported_abi(argv[2]);
  }
  if (argc >= 4 && std::strcmp(argv[1], "--empty") == 0) {
    return run_empty(argv[2], argv[3]);
  }
  if (argc >= 5 && std::strcmp(argv[1], "--fixture") == 0) {
    return run_fixture(argv[2], argv[3], argv[4]);
  }
  std::cerr << "Usage: pacman_discovery_provider_test "
               "--missing-lib | --unsupported-abi <bad.so> | "
               "--empty <root> <dbpath> | "
               "--fixture <root> <dbpath> <pacman.conf>\n";
  return 2;
}
