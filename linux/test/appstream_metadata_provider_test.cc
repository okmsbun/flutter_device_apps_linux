// Linux-only native integration test for AppStreamMetadataProvider and
// create_libappstream_backend() runtime GModule loading.
//
// Modes:
//   --missing-lib
//       Verifies graceful unavailability when libappstream is absent.
//   --unsupported-abi <incomplete.so> <mismatched-so5.so>
//       Verifies missing required symbols and SONAME/ABI mismatches are
//       rejected cleanly.
//   --fixture <metainfo_dir> <catalog_dir>
//       Runs real runtime-loaded libappstream.so.5 / libappstream.so.4 against
//       isolated local offline XML fixtures (no network access, no host
//       catalogs).

#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "discovery/application_discovery_service.h"
#include "discovery/appstream_metadata_provider.h"
#include "discovery/libappstream_backend.h"

namespace {

using flutter_device_apps_linux::ApplicationDiscoveryService;
using flutter_device_apps_linux::AppStreamDataLocation;
using flutter_device_apps_linux::AppStreamMetadataOriginKind;
using flutter_device_apps_linux::AppStreamMetadataProvider;
using flutter_device_apps_linux::AppStreamMetadataRecord;
using flutter_device_apps_linux::AppStreamMetadataResult;
using flutter_device_apps_linux::create_libappstream_backend;
using flutter_device_apps_linux::LibAppStreamConfig;

int g_failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

int run_missing_lib() {
  LibAppStreamConfig cfg;
  cfg.library_candidates = {"libappstream-does-not-exist.so.999"};
  AppStreamMetadataProvider provider(create_libappstream_backend(cfg));
  expect(!provider.is_available(),
         "missing libappstream must report is_available() == false");
  const AppStreamMetadataResult res = provider.load_metadata();
  expect(!res.ok, "load_metadata() with missing library must fail cleanly");
  if (g_failures > 0) return 1;
  std::cout << "AppStream missing-library native test passed.\n";
  return 0;
}

int run_unsupported_abi(const char* incomplete_so,
                        const char* mismatched_so5) {
  {
    LibAppStreamConfig cfg;
    cfg.library_candidates = {incomplete_so};
    AppStreamMetadataProvider provider(create_libappstream_backend(cfg));
    expect(!provider.is_available(),
           "library missing core symbols must report unavailable");
  }
  {
    LibAppStreamConfig cfg;
    cfg.library_candidates = {mismatched_so5};
    AppStreamMetadataProvider provider(create_libappstream_backend(cfg));
    expect(!provider.is_available(),
           "SONAME .so.5 without AppStream 1.0 AsComponentBox ABI must be "
           "rejected");
  }
  if (g_failures > 0) return 1;
  std::cout << "AppStream unsupported-ABI native test passed.\n";
  return 0;
}

int run_fixture(const char* metainfo_dir, const char* catalog_dir) {
  LibAppStreamConfig cfg;
  cfg.load_standard_locations = false;
  cfg.locale = "tr_TR";
  cfg.extra_locations.push_back(
      {metainfo_dir, AppStreamMetadataOriginKind::kMetainfo});
  cfg.extra_locations.push_back(
      {catalog_dir, AppStreamMetadataOriginKind::kCatalog});

  ApplicationDiscoveryService service;
  service.set_metadata_provider(std::make_unique<AppStreamMetadataProvider>(
      create_libappstream_backend(cfg)));

  // Confirm catalog-only and metainfo-only records never appear as installed
  // applications in discover_applications().
  expect(service.discover_applications().empty(),
         "AppStream fixtures must not appear in discover_applications()");

  const AppStreamMetadataResult res = service.load_appstream_metadata();
  expect(res.ok, "isolated local AppStream fixture load must succeed");

  int editor_candidates = 0;
  bool saw_localized_desktop = false;
  bool saw_console = false;

  for (const auto& rec : res.records) {
    expect(!rec.provenance.is_installation_proof,
           "loaded AppStream record must never claim installation proof");

    if (rec.component_id == "org.example.Editor") {
      ++editor_candidates;
      if (rec.component_kind == "desktop-application" &&
          rec.desktop_launchable_ids.size() == 1 &&
          rec.desktop_launchable_ids[0] == "org.example.Editor.desktop") {
        saw_localized_desktop = true;
        expect(rec.name.has_value() && rec.name.value() == "Örnek Editör",
               "localized Turkish name must be resolved");
        expect(rec.summary.has_value() &&
                   rec.summary.value() == "Metin düzenleyici",
               "localized Turkish summary must be resolved");
        expect(rec.project_license.has_value() &&
                   rec.project_license.value() == "MIT",
               "project_license must be parsed");
        expect(rec.homepage.has_value() &&
                   rec.homepage.value() == "https://example.org/editor",
               "homepage URL must be parsed");
        expect(!rec.categories.empty(), "categories must be parsed");
        expect(!rec.releases.empty() &&
                   rec.releases[0].version.has_value() &&
                   rec.releases[0].version.value() == "3.1.0",
               "release metadata must be parsed");
      }
    } else if (rec.component_id == "org.example.CliTool") {
      if (rec.component_kind == "console-application") {
        saw_console = true;
        expect(!rec.package_names.empty() &&
                   rec.package_names[0] == "example-cli",
               "package_names must be parsed from catalog");
        expect(!rec.bundles.empty() && rec.bundles[0].kind == "flatpak",
               "bundle identifiers must be parsed from catalog");
      }
    }
  }

  expect(saw_localized_desktop,
         "localized desktop-application fixture must be loaded");
  expect(saw_console, "catalog-only console-application fixture must be loaded");
  expect(editor_candidates >= 1,
         "org.example.Editor metadata candidate must be present");

  if (g_failures > 0) return 1;
  std::cout << "AppStream local fixture native integration test passed.\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--missing-lib") == 0) {
    return run_missing_lib();
  }
  if (argc >= 4 && std::strcmp(argv[1], "--unsupported-abi") == 0) {
    return run_unsupported_abi(argv[2], argv[3]);
  }
  if (argc >= 4 && std::strcmp(argv[1], "--fixture") == 0) {
    return run_fixture(argv[2], argv[3]);
  }
  std::cerr << "Usage: appstream_metadata_provider_test "
               "--missing-lib | --unsupported-abi <bad.so> <bad.so.5> | "
               "--fixture <metainfo_dir> <catalog_dir>\n";
  return 2;
}
