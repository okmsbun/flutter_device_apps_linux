#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LEGACY_LIST_APPS_ADAPTER_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LEGACY_LIST_APPS_ADAPTER_H_

#include <string>
#include <vector>

#include "discovered_application.h"

namespace flutter_device_apps_linux {

// Legacy projection of a discovered application matching the existing
// MethodChannel listApps() response schema.
struct LegacyDesktopAppRecord {
  std::string desktop_file_id;
  OptionalString app_name;
  bool supports_files = false;
  bool supports_uris = false;
  bool requires_terminal = false;
};

// Validates whether an optional desktop file ID satisfies the legacy listApps()
// contract (present, non-empty stem before ".desktop", no '/' or NUL bytes).
bool is_valid_legacy_desktop_file_id(const OptionalString& desktop_file_id);

// Maps unified discovery records into the legacy listApps() representation.
// Preserves the original GIO visible-application domain:
// - Includes only records discovered by the "gio" provider.
// - Includes only records with DesktopMenuVisibility::kVisible.
// - Includes only records satisfying is_valid_legacy_desktop_file_id().
// - Preserves relative ordering and exposes only legacy fields.
std::vector<LegacyDesktopAppRecord> adapt_for_legacy_list_apps(
    const std::vector<DiscoveredApplication>& applications);

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_LEGACY_LIST_APPS_ADAPTER_H_
