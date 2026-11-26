#include "legacy_list_apps_adapter.h"

#include <cstring>
#include <utility>

namespace flutter_device_apps_linux {
namespace {

const char kDesktopSuffix[] = ".desktop";
const char kGioProviderId[] = "gio";

}  // namespace

bool is_valid_legacy_desktop_file_id(const OptionalString& desktop_file_id) {
  if (!desktop_file_id.has_value()) return false;
  const std::string& id = desktop_file_id.value();
  const size_t suffix_length = std::strlen(kDesktopSuffix);
  if (id.size() <= suffix_length) return false;
  if (id.compare(id.size() - suffix_length, suffix_length, kDesktopSuffix) !=
      0) {
    return false;
  }
  if (id.find('/') != std::string::npos || id.find('\0') != std::string::npos) {
    return false;
  }
  return true;
}

std::vector<LegacyDesktopAppRecord> adapt_for_legacy_list_apps(
    const std::vector<DiscoveredApplication>& applications) {
  std::vector<LegacyDesktopAppRecord> records;
  records.reserve(applications.size());

  for (const auto& app : applications) {
    if (app.provenance.provider_id != kGioProviderId) continue;
    if (app.desktop_menu_visibility != DesktopMenuVisibility::kVisible) {
      continue;
    }
    if (!is_valid_legacy_desktop_file_id(app.desktop_file_id)) continue;

    LegacyDesktopAppRecord record;
    record.desktop_file_id = app.desktop_file_id.value();
    record.app_name = app.metadata.name;
    record.supports_files =
        app.capabilities.supports_files == CapabilityState::kSupported;
    record.supports_uris =
        app.capabilities.supports_uris == CapabilityState::kSupported;
    record.requires_terminal =
        app.capabilities.requires_terminal == CapabilityState::kSupported;
    records.push_back(std::move(record));
  }

  return records;
}

}  // namespace flutter_device_apps_linux
