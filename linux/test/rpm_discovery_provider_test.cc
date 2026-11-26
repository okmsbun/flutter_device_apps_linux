// Linux-only native integration test for RpmDiscoveryProvider and
// create_librpm_backend() runtime loading via GModule.
//
// Modes:
//   --missing-lib
//       Verifies graceful unavailability when librpm is absent.
//   --unsupported-abi <incomplete.so>
//       Verifies missing required librpm symbols report unavailable cleanly.
//   --empty <empty_rpm_root>
//       Queries an initialized empty isolated RPM database via rpmtsSetRootDir.
//   --fixture <populated_rpm_root>
//       Queries an isolated local RPM database with multi-version packages and
//       compares results against `rpm --root <root> -qa` (test-only oracle).

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <string>

#include "discovery/librpm_backend.h"
#include "discovery/rpm_discovery_provider.h"

namespace {

using flutter_device_apps_linux::ApplicationClassification;
using flutter_device_apps_linux::create_librpm_backend;
using flutter_device_apps_linux::InstallationScope;
using flutter_device_apps_linux::InstallationSource;
using flutter_device_apps_linux::LibRpmConfig;
using flutter_device_apps_linux::PackageDiscoveryResult;
using flutter_device_apps_linux::RpmDiscoveryProvider;

int g_failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

int run_missing_lib() {
  LibRpmConfig cfg;
  cfg.library_candidates = {"librpm-does-not-exist.so.999"};
  RpmDiscoveryProvider provider(create_librpm_backend(cfg));
  expect(!provider.is_available(),
         "missing librpm must report is_available() == false");
  const PackageDiscoveryResult res = provider.discover_packages();
  expect(!res.ok && res.error_message.find("failed to load") !=
                        std::string::npos,
         "missing librpm diagnostic must report dynamic load failure");
  if (g_failures > 0) return 1;
  std::cout << "RPM missing-library native test passed.\n";
  return 0;
}

int run_unsupported_abi(const char* incomplete_so) {
  LibRpmConfig cfg;
  cfg.library_candidates = {incomplete_so};
  RpmDiscoveryProvider provider(create_librpm_backend(cfg));
  expect(!provider.is_available(),
         "library missing required librpm symbols must report unavailable");
  const PackageDiscoveryResult res = provider.discover_packages();
  expect(!res.ok && res.error_message.find("missing required symbols") !=
                        std::string::npos,
         "missing required symbols diagnostic must be distinguishable");
  if (g_failures > 0) return 1;
  std::cout << "RPM unsupported-ABI native test passed.\n";
  return 0;
}

int run_empty(const char* empty_root) {
  LibRpmConfig cfg;
  cfg.root_dir = empty_root;
  RpmDiscoveryProvider provider(create_librpm_backend(cfg));
  if (!provider.is_available()) {
    const auto diag = provider.discover_packages();
    std::cerr << "FAIL: librpm unavailable: " << diag.error_message << "\n";
    return 1;
  }
  const PackageDiscoveryResult res = provider.discover_packages();
  expect(res.ok && res.packages.empty(),
         "empty isolated RPM database must succeed with 0 packages");
  if (g_failures > 0) return 1;
  std::cout << "RPM empty-database native integration test passed.\n";
  return 0;
}

int run_fixture(const char* populated_root) {
  LibRpmConfig cfg;
  cfg.root_dir = populated_root;
  RpmDiscoveryProvider provider(create_librpm_backend(cfg));
  if (!provider.is_available()) {
    const auto diag = provider.discover_packages();
    std::cerr << "FAIL: librpm unavailable: " << diag.error_message << "\n";
    return 1;
  }

  const PackageDiscoveryResult res = provider.discover_packages();
  expect(res.ok && res.error_message.empty(),
         "populated isolated RPM fixture discovery must succeed cleanly");

  std::set<std::string> parsed_tuples;
  std::set<std::string> unique_install_ids;
  int multi_ver_count = 0;
  std::string multi_ver_stable_id;

  for (const auto& pkg : res.packages) {
    expect(pkg.provenance.provider_id == "rpm" &&
               pkg.provenance.installation_source == InstallationSource::kRpm &&
               pkg.provenance.installation_scope ==
                   InstallationScope::kSystem &&
               pkg.classification == ApplicationClassification::kUnknown,
           "RPM provenance and unknown classification must match");
    expect(pkg.installation_identity_id.has_value(),
           "RPM record must carry version-specific installation_identity_id");
    if (pkg.installation_identity_id.has_value()) {
      unique_install_ids.insert(pkg.installation_identity_id.value());
    }

    const std::string epoch =
        pkg.epoch.has_value() ? pkg.epoch.value() : "(none)";
    const std::string ver = pkg.version.has_value() ? pkg.version.value() : "";
    const std::string rel = pkg.release.has_value() ? pkg.release.value() : "";
    const std::string arch =
        pkg.architecture.has_value() ? pkg.architecture.value() : "";
    parsed_tuples.insert(pkg.package_name + "\t" + epoch + "\t" + ver + "\t" +
                         rel + "\t" + arch);

    if (pkg.package_name == "example-multi") {
      ++multi_ver_count;
      if (multi_ver_stable_id.empty()) {
        multi_ver_stable_id = pkg.package_identity_id;
      } else {
        expect(pkg.package_identity_id == multi_ver_stable_id,
               "multi-version RPM package must share stable package_identity_id");
      }
      expect(pkg.license.has_value() && pkg.license.value() == "MIT",
             "RPM license tag must be populated");
      expect(pkg.vendor.has_value() && pkg.vendor.value() == "Example Vendor",
             "RPM vendor tag must be populated");
      expect(pkg.homepage.has_value() &&
                 pkg.homepage.value() == "https://example.org/multi",
             "RPM URL/homepage tag must be populated");
      expect(pkg.package_group.has_value() &&
                 pkg.package_group.value() == "Applications/System",
             "RPM group tag must be populated");
      expect(pkg.summary.has_value() && !pkg.summary.value().empty() &&
                 pkg.description.has_value() &&
                 !pkg.description.value().empty(),
             "RPM summary and description must be populated");
    }
  }

  expect(multi_ver_count == 2,
         "both installed versions of example-multi must be discovered");
  expect(unique_install_ids.size() == res.packages.size(),
         "all installed RPM versions must have distinct "
         "installation_identity_ids");

  // Compare against `rpm --root <populated_root> -qa` as test-only oracle.
  const std::string cmd =
      std::string("rpm --root '") + populated_root +
      "' -qa --qf '%{NAME}\\t%{EPOCH}\\t%{VERSION}\\t%{RELEASE}\\t%{ARCH}\\n'";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (pipe == nullptr) {
    std::cerr << "FAIL: unable to execute rpm -qa reference command\n";
    return 1;
  }

  std::set<std::string> expected_tuples;
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
    std::string line(buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    if (!line.empty()) {
      expected_tuples.insert(std::move(line));
    }
  }
  const int rc = pclose(pipe);
  expect(rc == 0, "rpm -qa reference command must exit 0");
  expect(!expected_tuples.empty(), "rpm -qa must return fixture packages");
  expect(parsed_tuples == expected_tuples,
         "RpmDiscoveryProvider output must match rpm -qa reference output");

  // --- Runtime file ownership via librpm basename index + rpmfi ---
  {
    auto own_backend = create_librpm_backend(cfg);
    const auto own = own_backend->find_file_owners(
        {"/usr/share/applications/example-tool.desktop",
         "/usr/share/applications/not-owned.desktop",
         "/other/dir/example-tool.desktop"});
    expect(own.ok, "librpm file ownership query must succeed");
    expect(own.owners.size() == 1 &&
               own.owners[0].path ==
                   "/usr/share/applications/example-tool.desktop" &&
               own.owners[0].package.name == "example-tool" &&
               own.owners[0].package.version.has_value() &&
               own.owners[0].package.version.value() == "3.1.4",
           "rpmfi full-path verification must own only the exact path (same "
           "basename in another directory must not match)");

    const std::string list_cmd =
        std::string("rpm --root '") + populated_root + "' -ql example-tool";
    FILE* list_pipe = popen(list_cmd.c_str(), "r");
    bool oracle_has_path = false;
    if (list_pipe != nullptr) {
      char line_buf[1024];
      while (std::fgets(line_buf, sizeof(line_buf), list_pipe) != nullptr) {
        std::string line(line_buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
          line.pop_back();
        }
        if (line == "/usr/share/applications/example-tool.desktop") {
          oracle_has_path = true;
        }
      }
      pclose(list_pipe);
    }
    expect(oracle_has_path,
           "rpm -ql reference must list the owned desktop file path");
  }

  if (g_failures > 0) return 1;
  std::cout << "RPM local fixture and rpm -qa parity test passed ("
            << parsed_tuples.size() << " packages matched, ABI="
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
  if (argc >= 3 && std::strcmp(argv[1], "--empty") == 0) {
    return run_empty(argv[2]);
  }
  if (argc >= 3 && std::strcmp(argv[1], "--fixture") == 0) {
    return run_fixture(argv[2]);
  }
  std::cerr << "Usage: rpm_discovery_provider_test "
               "--missing-lib | --unsupported-abi <bad.so> | "
               "--empty <root> | --fixture <root>\n";
  return 2;
}
