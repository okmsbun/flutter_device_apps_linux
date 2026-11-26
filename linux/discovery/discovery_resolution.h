#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_DISCOVERY_RESOLUTION_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_DISCOVERY_RESOLUTION_H_

#include <string>
#include <vector>

#include "appstream_metadata.h"
#include "discovered_application.h"
#include "installed_package.h"

namespace flutter_device_apps_linux {

struct ProviderExecutionSummary;
struct DiscoveryPassResult;

// Authoritative file-to-package ownership evidence returned by a documented
// package database lookup (for example, RPM RPMDBI_BASENAMES, libalpm file
// list, DPKG info lists, or Nix store path ownership).
struct PackageOwnershipEvidence {
  std::string package_provider_id;  // "dpkg", "rpm", "pacman", "nix"
  OptionalString package_identity_id;
  std::string package_name;
  OptionalString architecture;
  InstallationScope scope = InstallationScope::kSystem;
  OptionalString profile_path;
};

// Optional seam for querying documented package ownership databases for a
// specific file path (such as a .desktop file). Implementations must never use
// filesystem heuristics, recursive scanning, or shell commands.
class PackageOwnershipLookup {
 public:
  virtual ~PackageOwnershipLookup() = default;

  // Returns all authoritative package owners for `file_path`. Returns empty if
  // unknown, or multiple entries if ownership is shared/ambiguous.
  virtual std::vector<PackageOwnershipEvidence> lookup_file_owners(
      const std::string& file_path) const = 0;
};

// Preserves field-level metadata disagreements between authoritative sources
// without silently dropping conflicting values.
struct MetadataConflict {
  std::string field_name;
  std::string chosen_provider_id;
  std::string chosen_value;
  std::string conflicting_provider_id;
  std::string conflicting_value;
};

// Records an ambiguity preserved rather than guessed during correlation.
struct ResolutionAmbiguity {
  std::string reason;
  std::vector<std::string> candidate_ids;
};

// Desktop entry attached to a resolved application (supports multiple desktop
// entries belonging to a single installation or package).
struct ResolvedDesktopEntry {
  std::string desktop_file_id;
  OptionalString desktop_file_path;
  OptionalString display_name;
  DesktopMenuVisibility desktop_menu_visibility =
      DesktopMenuVisibility::kUnknown;
  ApplicationCapabilities capabilities;
  std::string provider_id;
  std::string original_application_id;
};

// Upstream AppStream catalog/metainfo release information kept strictly
// separate from the installed application/package version.
struct CatalogReleaseInfo {
  OptionalString latest_version;
  OptionalString latest_date;
  std::vector<AppStreamReleaseMetadata> releases;
};

// Normalized, deduplicated, enriched, and classified internal application
// record produced by the correlation layer.
struct ResolvedApplication {
  // Deterministic canonical identifier stable across upgrades and independent
  // of provider execution order.
  std::string resolved_id;

  // Primary installation provenance plus all contributing source provenances
  // and original provider-scoped record IDs.
  SourceProvenance primary_provenance;
  std::vector<SourceProvenance> source_provenances;
  std::vector<std::string> original_record_ids;

  // Attached desktop entries (0, 1, or many).
  std::vector<ResolvedDesktopEntry> desktop_entries;

  // Strongly correlated installed package identities.
  std::vector<std::string> linked_package_identity_ids;

  // Strongly correlated AppStream component IDs.
  std::vector<std::string> linked_appstream_component_ids;

  // Installed metadata (metadata.version holds ONLY installed version).
  ApplicationMetadata metadata;

  // Upstream catalog/metainfo release metadata (never merged into
  // metadata.version).
  CatalogReleaseInfo catalog_releases;

  // Aggregated capabilities and desktop menu visibility.
  ApplicationCapabilities capabilities;
  DesktopMenuVisibility desktop_menu_visibility =
      DesktopMenuVisibility::kUnknown;

  // Authoritative classification; stays kUnknown without strong evidence.
  ApplicationClassification classification =
      ApplicationClassification::kUnknown;

  // Preserved field-level metadata conflicts and ambiguities.
  std::vector<MetadataConflict> metadata_conflicts;
  std::vector<ResolutionAmbiguity> ambiguities;
};

// Output of cross-provider normalization, deduplication, enrichment, and
// classification. Confirmed applications, raw system packages, and unmatched
// catalog-only AppStream records remain strictly separated.
struct ResolvedDiscoveryInventory {
  std::vector<ResolvedApplication> applications;
  std::vector<InstalledPackageRecord> packages;
  std::vector<AppStreamMetadataRecord> unmatched_appstream_records;
  std::vector<ResolutionAmbiguity> ambiguities;
  std::vector<ProviderExecutionSummary> provider_summaries;
};

// Maps an authoritative AppStream component kind string to an internal
// ApplicationClassification. Never classifies from names or sections.
ApplicationClassification classify_appstream_component_kind(
    const std::string& component_kind);

// Correlates, deduplicates, enriches, and classifies discovery records across
// GIO, Flatpak, Snap, DPKG, RPM, Pacman, Nix, and AppStream using indexed
// matching (no quadratic all-pairs scans).
//
// Invariants:
// - Never merges records solely by display name, package name similarity,
//   executable filename, or bare application ID without matching source/scope.
// - Preserves separate installations across package managers, scopes, profiles,
//   architectures, branches, and parallel Snap instances.
// - Preserves ambiguous matches explicitly instead of guessing.
// - Never promotes catalog-only AppStream records into installed applications.
// - Keeps installed versions strictly separate from catalog release versions.
// - Output ordering and identities are deterministic and independent of
//   provider execution order.
ResolvedDiscoveryInventory resolve_discovery_inventory(
    const DiscoveryPassResult& pass_result,
    const std::vector<AppStreamMetadataRecord>& appstream_records = {},
    const PackageOwnershipLookup* ownership_lookup = nullptr);

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_DISCOVERY_RESOLUTION_H_
