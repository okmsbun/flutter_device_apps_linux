// Linux-only native integration test for FlatpakDiscoveryProvider and
// create_libflatpak_backend() runtime loading via GModule.
//
// Modes:
//   --missing-lib
//       Verifies graceful unavailability when libflatpak shared object does
//       not exist.
//   --missing-symbols <path-to-incomplete-so> <path-to-core-only-so>
//       Verifies missing required symbols disable the backend while missing
//       optional appdata symbols still allow discovery to be available.
//   --fixture
//       Runs real libflatpak.so.0 against isolated local FLATPAK_USER_DIR,
//       FLATPAK_SYSTEM_DIR, and FLATPAK_CONFIG_DIR installations built offline.

#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <string>

#include "discovery/discovered_application.h"
#include "discovery/flatpak_discovery_provider.h"
#include "discovery/legacy_list_apps_adapter.h"
#include "discovery/libflatpak_backend.h"

namespace {

using flutter_device_apps_linux::ApplicationClassification;
using flutter_device_apps_linux::CapabilityState;
using flutter_device_apps_linux::create_libflatpak_backend;
using flutter_device_apps_linux::DesktopMenuVisibility;
using flutter_device_apps_linux::DiscoveryResult;
using flutter_device_apps_linux::FlatpakDiscoveryProvider;
using flutter_device_apps_linux::InstallationScope;
using flutter_device_apps_linux::InstallationSource;

int g_failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

int run_missing_lib() {
  FlatpakDiscoveryProvider provider(
      create_libflatpak_backend("libflatpak-does-not-exist.so.999"));
  expect(!provider.is_available(),
         "missing libflatpak library must report is_available() == false");
  const DiscoveryResult result = provider.discover();
  expect(!result.ok, "discover() with missing library must fail cleanly");
  expect(result.error_message.find("failed to load") != std::string::npos,
         "missing library diagnostic must report dynamic loading error");
  if (g_failures > 0) return 1;
  std::cout << "Flatpak missing-library native test passed.\n";
  return 0;
}

int run_missing_symbols(const char* incomplete_so, const char* core_only_so) {
  {
    FlatpakDiscoveryProvider provider(create_libflatpak_backend(incomplete_so));
    expect(!provider.is_available(),
           "library missing required symbols must report unavailable");
    const DiscoveryResult result = provider.discover();
    expect(!result.ok && result.error_message.find("missing required symbols") !=
                             std::string::npos,
           "missing required symbols diagnostic must be distinguishable");
  }
  {
    FlatpakDiscoveryProvider provider(create_libflatpak_backend(core_only_so));
    expect(provider.is_available(),
           "library with required core symbols but without optional appdata "
           "symbols must report available");
  }
  if (g_failures > 0) return 1;
  std::cout << "Flatpak symbol-resolution native test passed.\n";
  return 0;
}

int run_fixture() {
  FlatpakDiscoveryProvider provider(
      create_libflatpak_backend("libflatpak.so.0"));
  if (!provider.is_available()) {
    const DiscoveryResult diag = provider.discover();
    std::cerr << "FAIL: libflatpak.so.0 unavailable: " << diag.error_message
              << "\n";
    ++g_failures;
    return 1;
  }

  const DiscoveryResult result = provider.discover();
  expect(result.ok, "isolated local Flatpak fixture discovery must succeed");
  expect(result.error_message.empty(),
         "healthy local Flatpak fixture must have no error diagnostics");

  bool saw_user_stable = false;
  bool saw_system_default = false;
  bool saw_system_extra = false;
  bool saw_runtime = false;
  std::set<std::string> unique_ids;

  for (const auto& app : result.applications) {
    unique_ids.insert(app.application_id);
    if (app.provenance.native_id == "org.example.Runtime") {
      saw_runtime = true;
    }
    if (app.provenance.native_id == "org.example.NoDesktopApp") {
      expect(app.provenance.provider_id == "flatpak",
             "provider_id must be flatpak");
      expect(app.provenance.installation_source == InstallationSource::kFlatpak,
             "installation_source must be kFlatpak");
      expect(!app.desktop_file_id.has_value(),
             "Flatpak app without desktop entry must not have desktop_file_id");
      expect(app.classification == ApplicationClassification::kUnknown,
             "classification must remain kUnknown");
      expect(app.desktop_menu_visibility == DesktopMenuVisibility::kUnknown,
             "desktop_menu_visibility must remain kUnknown");
      expect(app.capabilities.launchable == CapabilityState::kUnknown,
             "launchable must remain kUnknown");
      expect(app.metadata.architecture.has_value() &&
                 !app.metadata.architecture.value().empty(),
             "architecture must be populated");
      expect(app.metadata.branch.has_value() &&
                 app.metadata.branch.value() == "stable",
             "branch must be stable");

      if (app.provenance.installation_scope == InstallationScope::kUser) {
        saw_user_stable = true;
      } else if (app.provenance.installation_scope ==
                     InstallationScope::kSystem &&
                 app.installation_id.has_value()) {
        if (app.installation_id.value().find("default/") == 0) {
          saw_system_default = true;
        } else if (app.installation_id.value().find("extra/") == 0) {
          saw_system_extra = true;
        }
      }
    }
  }

  expect(!saw_runtime, "installed Flatpak runtime must be excluded");
  expect(saw_user_stable, "user Flatpak installation app must be discovered");
  expect(saw_system_default,
         "default system Flatpak installation app must be discovered");
  expect(saw_system_extra,
         "named extra system Flatpak installation app must be discovered");
  expect(unique_ids.size() == result.applications.size(),
         "all discovered Flatpak installations must have unique application_ids");

  const auto legacy =
      flutter_device_apps_linux::adapt_for_legacy_list_apps(result.applications);
  expect(legacy.empty(),
         "legacy listApps() adapter must exclude all Flatpak records");

  if (g_failures > 0) return 1;
  std::cout << "Flatpak local fixture native integration test passed.\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--missing-lib") == 0) {
    return run_missing_lib();
  }
  if (argc >= 4 && std::strcmp(argv[1], "--missing-symbols") == 0) {
    return run_missing_symbols(argv[2], argv[3]);
  }
  if (argc >= 2 && std::strcmp(argv[1], "--fixture") == 0) {
    return run_fixture();
  }
  std::cerr << "Usage: flatpak_discovery_provider_test "
               "--missing-lib | --missing-symbols <bad.so> <core.so> | "
               "--fixture\n";
  return 2;
}
