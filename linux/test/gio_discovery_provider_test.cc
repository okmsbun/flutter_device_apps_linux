// Linux-only native GIO integration test for GioDiscoveryProvider and
// adapt_for_legacy_list_apps().
//
// Run in an isolated process with controlled XDG_DATA_HOME, XDG_DATA_DIRS, and
// XDG_CURRENT_DESKTOP=GNOME set before process startup so GIO never depends on
// host applications or mutates environment variables after initialization.

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "discovery/discovered_application.h"
#include "discovery/gio_discovery_provider.h"
#include "discovery/legacy_list_apps_adapter.h"

namespace {

using flutter_device_apps_linux::ApplicationClassification;
using flutter_device_apps_linux::CapabilityState;
using flutter_device_apps_linux::DesktopMenuVisibility;
using flutter_device_apps_linux::DiscoveredApplication;
using flutter_device_apps_linux::DiscoveryResult;
using flutter_device_apps_linux::GioDiscoveryProvider;
using flutter_device_apps_linux::InstallationScope;
using flutter_device_apps_linux::InstallationSource;

int g_failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

const DiscoveredApplication* find_by_desktop_id(
    const std::vector<DiscoveredApplication>& apps, const std::string& id) {
  for (const auto& app : apps) {
    if (app.desktop_file_id.has_value() && app.desktop_file_id.value() == id) {
      return &app;
    }
  }
  return nullptr;
}

int run_empty_check() {
  GioDiscoveryProvider provider;
  expect(std::strcmp(provider.id(), "gio") == 0,
         "GioDiscoveryProvider id() must be gio");
  expect(provider.is_available(), "GioDiscoveryProvider must be available");

  const DiscoveryResult result = provider.discover();
  expect(result.ok, "empty GIO discovery must succeed");
  expect(result.applications.empty(),
         "isolated empty XDG environment must produce no apps");

  const auto legacy =
      flutter_device_apps_linux::adapt_for_legacy_list_apps(result.applications);
  expect(legacy.empty(), "legacy projection of empty discovery must be empty");

  if (g_failures > 0) return 1;
  std::cout << "GIO empty discovery native integration test passed.\n";
  return 0;
}

int run_fixture_check() {
  GioDiscoveryProvider provider;
  const DiscoveryResult result = provider.discover();
  expect(result.ok, "GIO fixture discovery must succeed");

  const auto* visible = find_by_desktop_id(
      result.applications, "org.example.VisibleFixture.desktop");
  const auto* no_display = find_by_desktop_id(
      result.applications, "org.example.NoDisplayFixture.desktop");
  const auto* only_kde = find_by_desktop_id(
      result.applications, "org.example.OnlyShowInKdeFixture.desktop");
  const auto* not_gnome = find_by_desktop_id(
      result.applications, "org.example.NotShowInGnomeFixture.desktop");
  const auto* hidden = find_by_desktop_id(
      result.applications, "org.example.HiddenFixture.desktop");

  expect(visible != nullptr, "normal visible entry must be discovered");
  if (visible != nullptr) {
    expect(visible->application_id == "gio:org.example.VisibleFixture.desktop",
           "visible entry must have deterministic gio application_id");
    expect(visible->provenance.provider_id == "gio",
           "visible provenance.provider_id must be gio");
    expect(visible->provenance.installation_source ==
               InstallationSource::kUnknown,
           "GIO must leave installation_source unknown");
    expect(visible->provenance.installation_scope ==
               InstallationScope::kUnknown,
           "GIO must leave installation_scope unknown");
    expect(visible->classification == ApplicationClassification::kUnknown,
           "GIO must leave classification unknown");
    expect(visible->desktop_menu_visibility == DesktopMenuVisibility::kVisible,
           "normal desktop entry must have kVisible visibility");
    expect(visible->metadata.name.has_value() &&
               visible->metadata.name.value() == "Visible Fixture",
           "visible entry display name must match");
    expect(visible->capabilities.supports_files == CapabilityState::kSupported,
           "visible entry supports_files must be kSupported");
    expect(visible->capabilities.supports_uris == CapabilityState::kUnsupported,
           "visible entry supports_uris must be kUnsupported");
    expect(visible->capabilities.requires_terminal ==
               CapabilityState::kUnsupported,
           "visible entry requires_terminal must be kUnsupported");
    expect(visible->capabilities.launchable == CapabilityState::kUnknown,
           "GIO must leave launchable unknown");
  }

  expect(no_display != nullptr,
         "NoDisplay=true entry must be retained by internal GIO discovery");
  if (no_display != nullptr) {
    expect(no_display->desktop_menu_visibility ==
               DesktopMenuVisibility::kNotVisible,
           "NoDisplay=true entry must be marked kNotVisible");
    expect(no_display->capabilities.requires_terminal ==
               CapabilityState::kSupported,
           "NoDisplay=true entry Terminal=true must be preserved");
  }

  expect(only_kde != nullptr,
         "OnlyShowIn=KDE entry must be retained by internal GIO discovery");
  if (only_kde != nullptr) {
    expect(only_kde->desktop_menu_visibility ==
               DesktopMenuVisibility::kNotVisible,
           "OnlyShowIn=KDE entry under GNOME must be marked kNotVisible");
  }

  expect(not_gnome != nullptr,
         "NotShowIn=GNOME entry must be retained by internal GIO discovery");
  if (not_gnome != nullptr) {
    expect(not_gnome->desktop_menu_visibility ==
               DesktopMenuVisibility::kNotVisible,
           "NotShowIn=GNOME entry under GNOME must be marked kNotVisible");
  }

  expect(hidden == nullptr,
         "Hidden=true entry must NOT be recovered beyond g_app_info_get_all()");

  // Verify legacy listApps() projection keeps only the visible entry.
  const auto legacy =
      flutter_device_apps_linux::adapt_for_legacy_list_apps(result.applications);
  expect(legacy.size() == 1,
         "legacy adapter must expose only the single visible desktop entry");
  if (legacy.size() == 1) {
    expect(legacy[0].desktop_file_id == "org.example.VisibleFixture.desktop",
           "legacy entry must be org.example.VisibleFixture.desktop");
    expect(legacy[0].app_name.has_value() &&
               legacy[0].app_name.value() == "Visible Fixture",
           "legacy entry app_name must match");
    expect(legacy[0].supports_files && !legacy[0].supports_uris &&
               !legacy[0].requires_terminal,
           "legacy capability booleans must match");
  }

  if (g_failures > 0) return 1;
  std::cout << "GIO fixture visibility native integration test passed.\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--empty") == 0) {
    return run_empty_check();
  }
  return run_fixture_check();
}
