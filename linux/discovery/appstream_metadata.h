#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPSTREAM_METADATA_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPSTREAM_METADATA_H_

#include <cstdint>
#include <string>
#include <vector>

#include "discovered_application.h"

namespace flutter_device_apps_linux {

// Distinguishes where an AppStream metadata record originated locally.
// Neither catalog entries nor metainfo files constitute authoritative proof
// that an application is installed or launchable.
enum class AppStreamMetadataOriginKind {
  kUnknown,
  kCatalog,
  kMetainfo,
};

// Packaging bundle identifier declared by an AppStream component
// (for example, kind="flatpak" or kind="snap").
struct AppStreamBundleReference {
  std::string kind;
  std::string id;
};

// Icon reference declared by an AppStream component.
// Remote URLs are preserved strictly as opaque strings and are never fetched.
struct AppStreamIconReference {
  std::string kind;
  std::string value;
};

// Upstream release entry from AppStream catalog or metainfo.
// Must NOT be treated as the installed version of an application.
struct AppStreamReleaseMetadata {
  OptionalString version;
  OptionalString date;
  int64_t timestamp = 0;
  bool has_timestamp = false;
  OptionalString urgency;
};

// Provenance of an AppStream metadata candidate.
struct AppStreamMetadataProvenance {
  std::string provider_id = "appstream";
  OptionalString origin;
  OptionalString data_id;
  InstallationScope scope = InstallationScope::kUnknown;
  AppStreamMetadataOriginKind origin_kind =
      AppStreamMetadataOriginKind::kUnknown;

  // Explicit invariant: AppStream metadata never proves installation.
  bool is_installation_proof = false;
};

// Internal metadata candidate loaded from local AppStream catalogs or metainfo.
// Kept strictly separate from confirmed installed DiscoveredApplication records
// so catalog-only software can never appear as installed.
struct AppStreamMetadataRecord {
  std::string component_id;
  std::string component_kind;

  OptionalString name;
  OptionalString summary;
  OptionalString description;
  OptionalString project_license;
  OptionalString homepage;

  // Explicit identifiers prepared for future cross-provider matching
  // (GIO, Flatpak, Snap, DPKG/RPM/Pacman) without matching on display names.
  std::vector<std::string> package_names;
  std::vector<AppStreamBundleReference> bundles;
  std::vector<std::string> desktop_launchable_ids;

  std::vector<std::string> categories;
  std::vector<AppStreamIconReference> icons;
  std::vector<AppStreamReleaseMetadata> releases;

  AppStreamMetadataProvenance provenance;
};

// Outcome of loading local AppStream metadata candidates.
// Preserves all ambiguous candidates (including duplicate component IDs from
// different origins/scopes/bundles) for future identity resolution.
struct AppStreamMetadataResult {
  std::vector<AppStreamMetadataRecord> records;
  bool ok = true;
  std::string error_message;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPSTREAM_METADATA_H_
