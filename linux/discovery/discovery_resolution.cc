#include "discovery_resolution.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "application_discovery_service.h"
#include "application_identity.h"

namespace flutter_device_apps_linux {
namespace {

std::string optional_or_empty(const OptionalString& value) {
  return value.has_value() ? value.value() : std::string();
}

int source_rank(InstallationSource source) {
  switch (source) {
    case InstallationSource::kFlatpak:
      return 1;
    case InstallationSource::kSnap:
      return 2;
    case InstallationSource::kDpkg:
      return 3;
    case InstallationSource::kRpm:
      return 4;
    case InstallationSource::kPacman:
      return 5;
    case InstallationSource::kNix:
      return 6;
    case InstallationSource::kAppImage:
      return 7;
    case InstallationSource::kUnknown:
      return 8;
  }
  return 9;
}

bool is_package_manager_source(InstallationSource source) {
  return source == InstallationSource::kDpkg ||
         source == InstallationSource::kRpm ||
         source == InstallationSource::kPacman ||
         source == InstallationSource::kNix;
}

bool is_scope_compatible(InstallationScope a, InstallationScope b) {
  return a == InstallationScope::kUnknown ||
         b == InstallationScope::kUnknown || a == b;
}

struct ParsedFlatpakBundle {
  std::string app_id;
  OptionalString arch;
  OptionalString branch;
};

// Parses AppStream flatpak bundle IDs such as:
//   "app/org.example.App/x86_64/stable" or "org.example.App".
ParsedFlatpakBundle parse_flatpak_bundle_id(const std::string& raw) {
  ParsedFlatpakBundle parsed;
  if (raw.empty()) return parsed;

  if (raw.compare(0, 4, "app/") == 0) {
    const size_t first_slash = 4;
    const size_t second_slash = raw.find('/', first_slash);
    if (second_slash == std::string::npos) {
      parsed.app_id = raw.substr(first_slash);
      return parsed;
    }
    parsed.app_id = raw.substr(first_slash, second_slash - first_slash);
    const size_t third_slash = raw.find('/', second_slash + 1);
    if (third_slash == std::string::npos) {
      const std::string tail = raw.substr(second_slash + 1);
      if (!tail.empty()) parsed.arch = OptionalString::of(tail);
      return parsed;
    }
    const std::string arch =
        raw.substr(second_slash + 1, third_slash - (second_slash + 1));
    const std::string branch = raw.substr(third_slash + 1);
    if (!arch.empty()) parsed.arch = OptionalString::of(arch);
    if (!branch.empty()) parsed.branch = OptionalString::of(branch);
    return parsed;
  }

  // Reject runtime/ bundles explicitly so runtimes never match Flatpak apps.
  if (raw.compare(0, 8, "runtime/") == 0) {
    return parsed;
  }
  parsed.app_id = raw;
  return parsed;
}

// Extracts "/nix/store/<entry>" prefix from a file path if it resides under
// /nix/store/. Returns empty string otherwise.
std::string extract_nix_store_root(const std::string& file_path) {
  static const char kPrefix[] = "/nix/store/";
  const size_t prefix_len = std::strlen(kPrefix);
  if (file_path.compare(0, prefix_len, kPrefix) != 0) return "";
  const size_t next_slash = file_path.find('/', prefix_len);
  if (next_slash == std::string::npos || next_slash == prefix_len) {
    return "";
  }
  return file_path.substr(0, next_slash);
}

void add_unique_string(std::vector<std::string>* target,
                       const std::string& value) {
  if (value.empty()) return;
  if (std::find(target->begin(), target->end(), value) == target->end()) {
    target->push_back(value);
  }
}

void add_unique_provenance(std::vector<SourceProvenance>* target,
                           const SourceProvenance& prov) {
  for (const auto& existing : *target) {
    if (existing.provider_id == prov.provider_id &&
        existing.native_id == prov.native_id &&
        existing.installation_source == prov.installation_source &&
        existing.installation_scope == prov.installation_scope) {
      return;
    }
  }
  target->push_back(prov);
}

void record_conflict(std::vector<MetadataConflict>* conflicts,
                     const std::string& field_name,
                     const std::string& chosen_provider,
                     const std::string& chosen_value,
                     const std::string& conflicting_provider,
                     const std::string& conflicting_value) {
  if (chosen_value == conflicting_value || conflicting_value.empty()) return;
  for (const auto& existing : *conflicts) {
    if (existing.field_name == field_name &&
        existing.chosen_provider_id == chosen_provider &&
        existing.chosen_value == chosen_value &&
        existing.conflicting_provider_id == conflicting_provider &&
        existing.conflicting_value == conflicting_value) {
      return;
    }
  }
  MetadataConflict conflict;
  conflict.field_name = field_name;
  conflict.chosen_provider_id = chosen_provider;
  conflict.chosen_value = chosen_value;
  conflict.conflicting_provider_id = conflicting_provider;
  conflict.conflicting_value = conflicting_value;
  conflicts->push_back(std::move(conflict));
}

// Tracks the winning provider + precedence rank for each optional metadata
// field of a ResolvedApplication so field-level precedence and conflicts stay
// completely independent of provider execution order.
struct FieldSourceTracker {
  struct Slot {
    int rank = -1;  // Higher rank wins.
    std::string provider_id;
  };
  Slot name;
  Slot version;
  Slot summary;
  Slot description;
  Slot license;
  Slot homepage;
  Slot executable;
  Slot icon_reference;
  Slot architecture;
  Slot branch;
  Slot revision;
  Slot channel;
};

void apply_metadata_field(const std::string& field_name,
                          const OptionalString& candidate,
                          const std::string& candidate_provider,
                          int candidate_rank, OptionalString* target,
                          FieldSourceTracker::Slot* slot,
                          std::vector<MetadataConflict>* conflicts) {
  if (!candidate.has_value() || candidate.value().empty()) return;

  if (!target->has_value() || target->value().empty()) {
    *target = candidate;
    slot->rank = candidate_rank;
    slot->provider_id = candidate_provider;
    return;
  }

  if (target->value() == candidate.value()) {
    if (candidate_rank > slot->rank) {
      slot->rank = candidate_rank;
      slot->provider_id = candidate_provider;
    }
    return;
  }

  if (candidate_rank > slot->rank ||
      (candidate_rank == slot->rank &&
       candidate_provider < slot->provider_id)) {
    const std::string prev_provider = slot->provider_id;
    const std::string prev_value = target->value();
    *target = candidate;
    slot->rank = candidate_rank;
    slot->provider_id = candidate_provider;
    record_conflict(conflicts, field_name, candidate_provider,
                    candidate.value(), prev_provider, prev_value);
  } else {
    record_conflict(conflicts, field_name, slot->provider_id, target->value(),
                    candidate_provider, candidate.value());
  }
}

// Precedence ranks per metadata field category:
// - name: GIO desktop entry (40) > AppStream metainfo (35) > AppStream catalog
//   (30) > Flatpak/Snap (20) > system package name (10)
// - summary / description: AppStream metainfo (40) > AppStream catalog (35) >
//   Flatpak/Snap (25) > DPKG/RPM/Pacman/Nix (15) > GIO (10)
// - license / homepage: installed package/Snap/Flatpak (35) > AppStream
//   metainfo (25) > AppStream catalog (20)
// - installed version: Flatpak/Snap (40) > DPKG/RPM/Pacman/Nix (35) > GIO (10)
//   (AppStream releases are NEVER applied to metadata.version!)
int name_rank_for_provider(const std::string& provider_id,
                           AppStreamMetadataOriginKind as_origin =
                               AppStreamMetadataOriginKind::kUnknown) {
  if (provider_id == "gio") return 40;
  if (provider_id == "appstream") {
    return as_origin == AppStreamMetadataOriginKind::kMetainfo ? 35 : 30;
  }
  if (provider_id == "flatpak" || provider_id == "snap") return 20;
  return 10;
}

int description_rank_for_provider(const std::string& provider_id,
                                  AppStreamMetadataOriginKind as_origin =
                                      AppStreamMetadataOriginKind::kUnknown) {
  if (provider_id == "appstream") {
    return as_origin == AppStreamMetadataOriginKind::kMetainfo ? 40 : 35;
  }
  if (provider_id == "flatpak" || provider_id == "snap") return 25;
  if (provider_id == "dpkg" || provider_id == "rpm" ||
      provider_id == "pacman" || provider_id == "nix") {
    return 15;
  }
  return 10;
}

int license_homepage_rank_for_provider(
    const std::string& provider_id,
    AppStreamMetadataOriginKind as_origin =
        AppStreamMetadataOriginKind::kUnknown) {
  if (provider_id == "dpkg" || provider_id == "rpm" ||
      provider_id == "pacman" || provider_id == "nix" ||
      provider_id == "snap" || provider_id == "flatpak") {
    return 35;
  }
  if (provider_id == "appstream") {
    return as_origin == AppStreamMetadataOriginKind::kMetainfo ? 25 : 20;
  }
  return 10;
}

int installed_version_rank_for_provider(const std::string& provider_id) {
  if (provider_id == "flatpak" || provider_id == "snap") return 40;
  if (provider_id == "dpkg" || provider_id == "rpm" ||
      provider_id == "pacman" || provider_id == "nix") {
    return 35;
  }
  return 10;
}

struct MutableResolvedEntry {
  ResolvedApplication app;
  FieldSourceTracker tracker;
  OptionalString flatpak_app_id;
  OptionalString snap_instance_name;
  OptionalString snap_package_name;
};

void recompute_desktop_aggregates(ResolvedApplication* app) {
  if (app->desktop_entries.empty()) return;

  std::sort(app->desktop_entries.begin(), app->desktop_entries.end(),
            [](const ResolvedDesktopEntry& a, const ResolvedDesktopEntry& b) {
              return std::tie(a.desktop_file_id, a.original_application_id) <
                     std::tie(b.desktop_file_id, b.original_application_id);
            });

  bool any_visible = false;
  bool any_not_visible = false;
  bool any_files = false;
  bool any_no_files = false;
  bool any_uris = false;
  bool any_no_uris = false;
  bool any_terminal = false;
  bool any_non_terminal = false;
  bool any_launchable = false;
  bool any_not_launchable = false;

  for (const auto& entry : app->desktop_entries) {
    if (entry.desktop_menu_visibility == DesktopMenuVisibility::kVisible) {
      any_visible = true;
    } else if (entry.desktop_menu_visibility ==
               DesktopMenuVisibility::kNotVisible) {
      any_not_visible = true;
    }
    if (entry.capabilities.supports_files == CapabilityState::kSupported) {
      any_files = true;
    } else if (entry.capabilities.supports_files ==
               CapabilityState::kUnsupported) {
      any_no_files = true;
    }
    if (entry.capabilities.supports_uris == CapabilityState::kSupported) {
      any_uris = true;
    } else if (entry.capabilities.supports_uris ==
               CapabilityState::kUnsupported) {
      any_no_uris = true;
    }
    if (entry.capabilities.requires_terminal == CapabilityState::kSupported) {
      any_terminal = true;
    } else if (entry.capabilities.requires_terminal ==
               CapabilityState::kUnsupported) {
      any_non_terminal = true;
    }
    if (entry.capabilities.launchable == CapabilityState::kSupported) {
      any_launchable = true;
    } else if (entry.capabilities.launchable == CapabilityState::kUnsupported) {
      any_not_launchable = true;
    }
  }

  app->desktop_menu_visibility = any_visible
                                     ? DesktopMenuVisibility::kVisible
                                     : (any_not_visible
                                            ? DesktopMenuVisibility::kNotVisible
                                            : DesktopMenuVisibility::kUnknown);
  app->capabilities.supports_files =
      any_files ? CapabilityState::kSupported
                : (any_no_files ? CapabilityState::kUnsupported
                                : CapabilityState::kUnknown);
  app->capabilities.supports_uris =
      any_uris ? CapabilityState::kSupported
               : (any_no_uris ? CapabilityState::kUnsupported
                              : CapabilityState::kUnknown);
  // If any attached desktop entry is graphical (Terminal=false), the
  // application does not strictly require a terminal.
  app->capabilities.requires_terminal =
      any_non_terminal ? CapabilityState::kUnsupported
                       : (any_terminal ? CapabilityState::kSupported
                                       : CapabilityState::kUnknown);
  app->capabilities.launchable =
      any_launchable ? CapabilityState::kSupported
                     : (any_not_launchable ? CapabilityState::kUnsupported
                                           : CapabilityState::kUnknown);
}

void attach_desktop_entry(MutableResolvedEntry* target,
                          const DiscoveredApplication& gio_app) {
  add_unique_provenance(&target->app.source_provenances, gio_app.provenance);
  add_unique_string(&target->app.original_record_ids, gio_app.application_id);

  if (gio_app.desktop_file_id.has_value() &&
      !gio_app.desktop_file_id.value().empty()) {
    bool already_present = false;
    for (const auto& existing : target->app.desktop_entries) {
      if (existing.desktop_file_id == gio_app.desktop_file_id.value() &&
          existing.original_application_id == gio_app.application_id) {
        already_present = true;
        break;
      }
    }
    if (!already_present) {
      ResolvedDesktopEntry entry;
      entry.desktop_file_id = gio_app.desktop_file_id.value();
      entry.desktop_file_path = gio_app.desktop_file_path;
      entry.display_name = gio_app.metadata.name;
      entry.desktop_menu_visibility = gio_app.desktop_menu_visibility;
      entry.capabilities = gio_app.capabilities;
      entry.provider_id = gio_app.provenance.provider_id;
      entry.original_application_id = gio_app.application_id;
      target->app.desktop_entries.push_back(std::move(entry));
    }
  }

  const std::string& prov = gio_app.provenance.provider_id;
  apply_metadata_field("name", gio_app.metadata.name, prov,
                       name_rank_for_provider(prov), &target->app.metadata.name,
                       &target->tracker.name, &target->app.metadata_conflicts);
  apply_metadata_field("summary", gio_app.metadata.summary, prov,
                       description_rank_for_provider(prov),
                       &target->app.metadata.summary, &target->tracker.summary,
                       &target->app.metadata_conflicts);
  apply_metadata_field(
      "description", gio_app.metadata.description, prov,
      description_rank_for_provider(prov), &target->app.metadata.description,
      &target->tracker.description, &target->app.metadata_conflicts);
  apply_metadata_field("version", gio_app.metadata.version, prov,
                       installed_version_rank_for_provider(prov),
                       &target->app.metadata.version, &target->tracker.version,
                       &target->app.metadata_conflicts);
  apply_metadata_field("executable", gio_app.metadata.executable, prov, 20,
                       &target->app.metadata.executable,
                       &target->tracker.executable,
                       &target->app.metadata_conflicts);

  recompute_desktop_aggregates(&target->app);
}

void merge_app_into_entry(MutableResolvedEntry* target,
                          const DiscoveredApplication& app) {
  add_unique_provenance(&target->app.source_provenances, app.provenance);
  add_unique_string(&target->app.original_record_ids, app.application_id);

  const std::string& prov = app.provenance.provider_id;
  apply_metadata_field("name", app.metadata.name, prov,
                       name_rank_for_provider(prov), &target->app.metadata.name,
                       &target->tracker.name, &target->app.metadata_conflicts);
  apply_metadata_field("version", app.metadata.version, prov,
                       installed_version_rank_for_provider(prov),
                       &target->app.metadata.version, &target->tracker.version,
                       &target->app.metadata_conflicts);
  apply_metadata_field("summary", app.metadata.summary, prov,
                       description_rank_for_provider(prov),
                       &target->app.metadata.summary, &target->tracker.summary,
                       &target->app.metadata_conflicts);
  apply_metadata_field(
      "description", app.metadata.description, prov,
      description_rank_for_provider(prov), &target->app.metadata.description,
      &target->tracker.description, &target->app.metadata_conflicts);
  apply_metadata_field("license", app.metadata.license, prov,
                       license_homepage_rank_for_provider(prov),
                       &target->app.metadata.license, &target->tracker.license,
                       &target->app.metadata_conflicts);
  apply_metadata_field(
      "homepage", app.metadata.homepage, prov,
      license_homepage_rank_for_provider(prov), &target->app.metadata.homepage,
      &target->tracker.homepage, &target->app.metadata_conflicts);
  apply_metadata_field("architecture", app.metadata.architecture, prov, 30,
                       &target->app.metadata.architecture,
                       &target->tracker.architecture,
                       &target->app.metadata_conflicts);
  apply_metadata_field("branch", app.metadata.branch, prov, 30,
                       &target->app.metadata.branch, &target->tracker.branch,
                       &target->app.metadata_conflicts);
  apply_metadata_field("revision", app.metadata.revision, prov, 30,
                       &target->app.metadata.revision,
                       &target->tracker.revision,
                       &target->app.metadata_conflicts);
  apply_metadata_field("channel", app.metadata.channel, prov, 30,
                       &target->app.metadata.channel, &target->tracker.channel,
                       &target->app.metadata_conflicts);
  apply_metadata_field("executable", app.metadata.executable, prov, 20,
                       &target->app.metadata.executable,
                       &target->tracker.executable,
                       &target->app.metadata_conflicts);
  apply_metadata_field("icon_reference", app.metadata.icon_reference, prov, 20,
                       &target->app.metadata.icon_reference,
                       &target->tracker.icon_reference,
                       &target->app.metadata_conflicts);

  if (app.desktop_file_id.has_value() && !app.desktop_file_id.value().empty()) {
    attach_desktop_entry(target, app);
  }
}

void enrich_from_package(MutableResolvedEntry* target,
                         const InstalledPackageRecord& pkg) {
  add_unique_string(&target->app.linked_package_identity_ids,
                    pkg.package_identity_id);
  add_unique_provenance(&target->app.source_provenances, pkg.provenance);
  add_unique_string(&target->app.original_record_ids, pkg.package_identity_id);

  if (target->app.primary_provenance.installation_source ==
      InstallationSource::kUnknown) {
    target->app.primary_provenance.installation_source =
        pkg.provenance.installation_source;
  }
  if (target->app.primary_provenance.installation_scope ==
      InstallationScope::kUnknown) {
    target->app.primary_provenance.installation_scope =
        pkg.provenance.installation_scope;
  }

  const std::string& prov = pkg.provenance.provider_id;
  // Combine RPM epoch/version/release or use authoritative package version.
  OptionalString installed_ver = pkg.version;
  if (pkg.provenance.installation_source == InstallationSource::kRpm &&
      pkg.version.has_value()) {
    std::string evr;
    if (pkg.epoch.has_value() && !pkg.epoch.value().empty()) {
      evr.append(pkg.epoch.value()).push_back(':');
    }
    evr.append(pkg.version.value());
    if (pkg.release.has_value() && !pkg.release.value().empty()) {
      evr.push_back('-');
      evr.append(pkg.release.value());
    }
    installed_ver = OptionalString::of(evr);
  }

  apply_metadata_field("version", installed_ver, prov,
                       installed_version_rank_for_provider(prov),
                       &target->app.metadata.version, &target->tracker.version,
                       &target->app.metadata_conflicts);
  apply_metadata_field("summary", pkg.summary, prov,
                       description_rank_for_provider(prov),
                       &target->app.metadata.summary, &target->tracker.summary,
                       &target->app.metadata_conflicts);
  apply_metadata_field(
      "description", pkg.description, prov, description_rank_for_provider(prov),
      &target->app.metadata.description, &target->tracker.description,
      &target->app.metadata_conflicts);
  apply_metadata_field("homepage", pkg.homepage, prov,
                       license_homepage_rank_for_provider(prov),
                       &target->app.metadata.homepage,
                       &target->tracker.homepage,
                       &target->app.metadata_conflicts);

  OptionalString pkg_license = pkg.license;
  if (!pkg_license.has_value() && !pkg.licenses.empty()) {
    std::string joined;
    for (size_t i = 0; i < pkg.licenses.size(); ++i) {
      if (i > 0) joined.append(" AND ");
      joined.append(pkg.licenses[i]);
    }
    pkg_license = OptionalString::of(joined);
  }
  apply_metadata_field("license", pkg_license, prov,
                       license_homepage_rank_for_provider(prov),
                       &target->app.metadata.license, &target->tracker.license,
                       &target->app.metadata_conflicts);
  apply_metadata_field("architecture", pkg.architecture, prov, 30,
                       &target->app.metadata.architecture,
                       &target->tracker.architecture,
                       &target->app.metadata_conflicts);
}

void enrich_from_appstream(MutableResolvedEntry* target,
                           const AppStreamMetadataRecord& rec) {
  add_unique_string(&target->app.linked_appstream_component_ids,
                    rec.component_id);

  const AppStreamMetadataOriginKind origin = rec.provenance.origin_kind;
  const std::string prov_label =
      origin == AppStreamMetadataOriginKind::kMetainfo ? "appstream:metainfo"
                                                       : "appstream:catalog";

  apply_metadata_field("name", rec.name, prov_label,
                       name_rank_for_provider("appstream", origin),
                       &target->app.metadata.name, &target->tracker.name,
                       &target->app.metadata_conflicts);
  apply_metadata_field("summary", rec.summary, prov_label,
                       description_rank_for_provider("appstream", origin),
                       &target->app.metadata.summary, &target->tracker.summary,
                       &target->app.metadata_conflicts);
  apply_metadata_field("description", rec.description, prov_label,
                       description_rank_for_provider("appstream", origin),
                       &target->app.metadata.description,
                       &target->tracker.description,
                       &target->app.metadata_conflicts);
  apply_metadata_field("license", rec.project_license, prov_label,
                       license_homepage_rank_for_provider("appstream", origin),
                       &target->app.metadata.license, &target->tracker.license,
                       &target->app.metadata_conflicts);
  apply_metadata_field("homepage", rec.homepage, prov_label,
                       license_homepage_rank_for_provider("appstream", origin),
                       &target->app.metadata.homepage,
                       &target->tracker.homepage,
                       &target->app.metadata_conflicts);

  // Keep upstream catalog release versions strictly separate from installed
  // metadata.version!
  if (!rec.releases.empty()) {
    for (const auto& rel : rec.releases) {
      target->app.catalog_releases.releases.push_back(rel);
    }
    if (!target->app.catalog_releases.latest_version.has_value() &&
        rec.releases.front().version.has_value()) {
      target->app.catalog_releases.latest_version =
          rec.releases.front().version;
      target->app.catalog_releases.latest_date = rec.releases.front().date;
    }
  }

  // Apply authoritative AppStream classification when available.
  const ApplicationClassification as_class =
      classify_appstream_component_kind(rec.component_kind);
  if (as_class != ApplicationClassification::kUnknown &&
      target->app.classification == ApplicationClassification::kUnknown) {
    target->app.classification = as_class;
  }
}

bool appstream_compatible_with_app(const AppStreamMetadataRecord& rec,
                                   const MutableResolvedEntry& entry) {
  if (!is_scope_compatible(rec.provenance.scope,
                           entry.app.primary_provenance.installation_scope)) {
    return false;
  }

  const InstallationSource src =
      entry.app.primary_provenance.installation_source;
  if (src == InstallationSource::kFlatpak && entry.flatpak_app_id.has_value()) {
    bool has_any_bundle = !rec.bundles.empty();
    bool has_matching_flatpak = false;
    for (const auto& b : rec.bundles) {
      if (b.kind != "flatpak") continue;
      const ParsedFlatpakBundle parsed = parse_flatpak_bundle_id(b.id);
      if (parsed.app_id != entry.flatpak_app_id.value()) continue;
      if (parsed.arch.has_value() &&
          entry.app.metadata.architecture.has_value() &&
          parsed.arch.value() != entry.app.metadata.architecture.value()) {
        continue;
      }
      if (parsed.branch.has_value() && entry.app.metadata.branch.has_value() &&
          parsed.branch.value() != entry.app.metadata.branch.value()) {
        continue;
      }
      has_matching_flatpak = true;
      break;
    }
    if (has_any_bundle && !has_matching_flatpak) return false;
  } else if (src == InstallationSource::kSnap &&
             entry.snap_package_name.has_value()) {
    bool has_any_bundle = !rec.bundles.empty();
    bool has_matching_snap = false;
    for (const auto& b : rec.bundles) {
      if (b.kind == "snap" && b.id == entry.snap_package_name.value()) {
        has_matching_snap = true;
        break;
      }
    }
    if (has_any_bundle && !has_matching_snap) return false;
  } else if (is_package_manager_source(src)) {
    // Reject Flatpak-only / Snap-only bundle records when matching distro
    // packages.
    if (!rec.bundles.empty() && rec.package_names.empty()) {
      return false;
    }
  }
  return true;
}

// Selects unique component matches from candidate AppStream indices.
// If all candidates share a single component_id, returns them ordered with
// kMetainfo before kCatalog. If multiple distinct component_ids exist, signals
// ambiguity instead of guessing.
bool select_unambiguous_appstream(
    const std::vector<size_t>& raw_candidates,
    const std::vector<AppStreamMetadataRecord>& records,
    std::vector<size_t>* selected_out, std::vector<std::string>* ambiguous_ids) {
  selected_out->clear();
  ambiguous_ids->clear();
  if (raw_candidates.empty()) return false;

  std::set<std::string> distinct_ids;
  std::vector<size_t> deduped;
  std::unordered_set<size_t> seen_idx;
  for (size_t idx : raw_candidates) {
    if (!seen_idx.insert(idx).second) continue;
    distinct_ids.insert(records[idx].component_id);
    deduped.push_back(idx);
  }

  if (distinct_ids.size() > 1) {
    for (const auto& cid : distinct_ids) {
      ambiguous_ids->push_back(cid);
    }
    return false;
  }

  // Order kMetainfo ahead of kCatalog deterministically.
  std::sort(deduped.begin(), deduped.end(), [&](size_t a, size_t b) {
    const int rank_a =
        records[a].provenance.origin_kind ==
                AppStreamMetadataOriginKind::kMetainfo
            ? 0
            : 1;
    const int rank_b =
        records[b].provenance.origin_kind ==
                AppStreamMetadataOriginKind::kMetainfo
            ? 0
            : 1;
    if (rank_a != rank_b) return rank_a < rank_b;
    return a < b;
  });
  *selected_out = std::move(deduped);
  return true;
}

}  // namespace

ApplicationClassification classify_appstream_component_kind(
    const std::string& component_kind) {
  if (component_kind == "desktop-application" ||
      component_kind == "web-application") {
    return ApplicationClassification::kGuiApplication;
  }
  if (component_kind == "console-application") {
    return ApplicationClassification::kCliApplication;
  }
  if (component_kind == "service") {
    return ApplicationClassification::kService;
  }
  if (component_kind == "runtime") {
    return ApplicationClassification::kRuntime;
  }
  if (component_kind == "library") {
    return ApplicationClassification::kLibrary;
  }
  return ApplicationClassification::kUnknown;
}

ResolvedDiscoveryInventory resolve_discovery_inventory(
    const DiscoveryPassResult& pass_result,
    const std::vector<AppStreamMetadataRecord>& appstream_records,
    const PackageOwnershipLookup* ownership_lookup) {
  ResolvedDiscoveryInventory inventory;
  inventory.provider_summaries = pass_result.provider_summaries;

  // 1. Canonical pre-sorting guarantees 100% deterministic output independent of
  // provider execution order.
  std::vector<DiscoveredApplication> sorted_apps = pass_result.applications;
  std::sort(
      sorted_apps.begin(), sorted_apps.end(),
      [](const DiscoveredApplication& a, const DiscoveredApplication& b) {
        return std::make_tuple(source_rank(a.provenance.installation_source),
                               a.provenance.provider_id, a.application_id,
                               optional_or_empty(a.desktop_file_id),
                               optional_or_empty(a.installation_id)) <
               std::make_tuple(source_rank(b.provenance.installation_source),
                               b.provenance.provider_id, b.application_id,
                               optional_or_empty(b.desktop_file_id),
                               optional_or_empty(b.installation_id));
      });

  inventory.packages = pass_result.packages;
  std::sort(inventory.packages.begin(), inventory.packages.end(),
            [](const InstalledPackageRecord& a,
               const InstalledPackageRecord& b) {
              return std::make_tuple(
                         a.package_identity_id,
                         optional_or_empty(a.installation_identity_id),
                         optional_or_empty(a.version),
                         a.provenance.provider_id) <
                     std::make_tuple(
                         b.package_identity_id,
                         optional_or_empty(b.installation_identity_id),
                         optional_or_empty(b.version),
                         b.provenance.provider_id);
            });

  std::vector<AppStreamMetadataRecord> sorted_as = appstream_records;
  std::sort(
      sorted_as.begin(), sorted_as.end(),
      [](const AppStreamMetadataRecord& a, const AppStreamMetadataRecord& b) {
        return std::make_tuple(a.component_id,
                               static_cast<int>(a.provenance.origin_kind),
                               optional_or_empty(a.provenance.origin),
                               optional_or_empty(a.provenance.data_id),
                               a.component_kind) <
               std::make_tuple(b.component_id,
                               static_cast<int>(b.provenance.origin_kind),
                               optional_or_empty(b.provenance.origin),
                               optional_or_empty(b.provenance.data_id),
                               b.component_kind);
      });

  // 2. Build indexed lookup maps for installed packages (O(P)).
  std::unordered_map<std::string, std::vector<size_t>> pkg_by_identity;
  std::unordered_map<std::string, std::vector<size_t>> pkg_by_provider_name;
  std::unordered_map<std::string, std::vector<size_t>> pkg_by_name;
  std::unordered_map<std::string, std::vector<size_t>> nix_by_store_path;

  pkg_by_identity.reserve(inventory.packages.size());
  pkg_by_name.reserve(inventory.packages.size());
  for (size_t i = 0; i < inventory.packages.size(); ++i) {
    const auto& pkg = inventory.packages[i];
    pkg_by_identity[pkg.package_identity_id].push_back(i);
    pkg_by_provider_name[pkg.provenance.provider_id + "\n" + pkg.package_name]
        .push_back(i);
    pkg_by_name[pkg.package_name].push_back(i);
    if (pkg.provenance.installation_source == InstallationSource::kNix) {
      for (const auto& sp : pkg.store_paths) {
        nix_by_store_path[sp].push_back(i);
      }
    }
  }

  // 3. Seed ResolvedApplications from authoritative runtime providers first
  // (Flatpak, Snap, and other non-GIO providers) while deduplicating exact
  // duplicate application_ids in O(1).
  std::vector<MutableResolvedEntry> entries;
  entries.reserve(sorted_apps.size());
  std::unordered_map<std::string, size_t> entry_by_resolved_id;
  std::unordered_map<std::string, std::vector<size_t>> flatpak_by_app_id;
  std::unordered_map<std::string, std::vector<size_t>> snap_by_instance;
  std::vector<const DiscoveredApplication*> deferred_gio;

  for (const auto& app : sorted_apps) {
    if (app.provenance.provider_id == "gio") {
      deferred_gio.push_back(&app);
      continue;
    }

    const auto existing = entry_by_resolved_id.find(app.application_id);
    if (existing != entry_by_resolved_id.end()) {
      merge_app_into_entry(&entries[existing->second], app);
      continue;
    }

    MutableResolvedEntry entry;
    entry.app.resolved_id = app.application_id;
    entry.app.primary_provenance = app.provenance;
    entry.app.classification = app.classification;
    if (app.provenance.installation_source == InstallationSource::kFlatpak) {
      entry.flatpak_app_id = OptionalString::of(app.provenance.native_id);
    } else if (app.provenance.installation_source ==
               InstallationSource::kSnap) {
      entry.snap_instance_name = OptionalString::of(app.provenance.native_id);
      entry.snap_package_name = app.package_id.has_value()
                                    ? app.package_id
                                    : OptionalString::of(
                                          app.provenance.native_id);
    }
    merge_app_into_entry(&entry, app);

    const size_t idx = entries.size();
    entry_by_resolved_id[entry.app.resolved_id] = idx;
    if (entry.flatpak_app_id.has_value()) {
      flatpak_by_app_id[entry.flatpak_app_id.value()].push_back(idx);
    }
    if (entry.snap_instance_name.has_value()) {
      snap_by_instance[entry.snap_instance_name.value()].push_back(idx);
    }
    entries.push_back(std::move(entry));
  }

  // 4. Correlate GIO desktop entries with Flatpak/Snap installations strictly
  // via authoritative X-Flatpak / X-SnapInstanceName markers, or create
  // standalone desktop-backed entries (preserving ambiguities!).
  for (const DiscoveredApplication* gio_ptr : deferred_gio) {
    const DiscoveredApplication& gio = *gio_ptr;

    // 4a. GIO -> Flatpak via authoritative X-Flatpak marker.
    if (gio.provenance.installation_source == InstallationSource::kFlatpak &&
        gio.package_id.has_value() && !gio.package_id.value().empty()) {
      const auto it = flatpak_by_app_id.find(gio.package_id.value());
      if (it != flatpak_by_app_id.end()) {
        std::vector<size_t> compatible;
        for (size_t idx : it->second) {
          const auto& candidate = entries[idx];
          if (!is_scope_compatible(
                  gio.provenance.installation_scope,
                  candidate.app.primary_provenance.installation_scope)) {
            continue;
          }
          if (gio.installation_id.has_value() &&
              candidate.app.resolved_id.find(gio.installation_id.value()) ==
                  std::string::npos) {
            continue;
          }
          compatible.push_back(idx);
        }
        if (compatible.size() == 1) {
          attach_desktop_entry(&entries[compatible.front()], gio);
          continue;
        }
        if (compatible.size() > 1) {
          // Ambiguous: multiple Flatpak installations (e.g. user+system or
          // stable+beta) match X-Flatpak without disambiguating scope/branch.
          // Preserve GIO entry separately with an explicit ambiguity record!
          ResolutionAmbiguity amb;
          amb.reason =
              "ambiguous Flatpak installation for desktop entry " +
              optional_or_empty(gio.desktop_file_id) + " (X-Flatpak=" +
              gio.package_id.value() + ")";
          for (size_t idx : compatible) {
            amb.candidate_ids.push_back(entries[idx].app.resolved_id);
          }

          MutableResolvedEntry standalone;
          standalone.app.resolved_id = gio.application_id;
          standalone.app.primary_provenance = gio.provenance;
          standalone.app.classification = gio.classification;
          standalone.flatpak_app_id = gio.package_id;
          standalone.app.ambiguities.push_back(amb);
          attach_desktop_entry(&standalone, gio);
          entry_by_resolved_id[standalone.app.resolved_id] = entries.size();
          entries.push_back(std::move(standalone));
          inventory.ambiguities.push_back(std::move(amb));
          continue;
        }
      }
    }

    // 4b. GIO -> Snap via authoritative X-SnapInstanceName marker.
    if (gio.provenance.installation_source == InstallationSource::kSnap &&
        gio.package_id.has_value() && !gio.package_id.value().empty()) {
      const auto it = snap_by_instance.find(gio.package_id.value());
      if (it != snap_by_instance.end()) {
        if (it->second.size() == 1) {
          attach_desktop_entry(&entries[it->second.front()], gio);
          continue;
        }
        if (it->second.size() > 1) {
          ResolutionAmbiguity amb;
          amb.reason =
              "ambiguous Snap installation for desktop entry " +
              optional_or_empty(gio.desktop_file_id) +
              " (X-SnapInstanceName=" + gio.package_id.value() + ")";
          for (size_t idx : it->second) {
            amb.candidate_ids.push_back(entries[idx].app.resolved_id);
          }
          MutableResolvedEntry standalone;
          standalone.app.resolved_id = gio.application_id;
          standalone.app.primary_provenance = gio.provenance;
          standalone.app.classification = gio.classification;
          standalone.snap_instance_name = gio.package_id;
          standalone.app.ambiguities.push_back(amb);
          attach_desktop_entry(&standalone, gio);
          entry_by_resolved_id[standalone.app.resolved_id] = entries.size();
          entries.push_back(std::move(standalone));
          inventory.ambiguities.push_back(std::move(amb));
          continue;
        }
      }
    }

    // 4c. Standalone GIO entry (or duplicate exact GIO application_id).
    const auto existing = entry_by_resolved_id.find(gio.application_id);
    if (existing != entry_by_resolved_id.end()) {
      attach_desktop_entry(&entries[existing->second], gio);
      continue;
    }

    MutableResolvedEntry entry;
    entry.app.resolved_id = gio.application_id;
    entry.app.primary_provenance = gio.provenance;
    entry.app.classification = gio.classification;
    if (gio.provenance.installation_source == InstallationSource::kFlatpak &&
        gio.package_id.has_value()) {
      entry.flatpak_app_id = gio.package_id;
    } else if (gio.provenance.installation_source ==
                   InstallationSource::kSnap &&
               gio.package_id.has_value()) {
      entry.snap_instance_name = gio.package_id;
      entry.snap_package_name = gio.package_id;
    }
    attach_desktop_entry(&entry, gio);
    entry_by_resolved_id[entry.app.resolved_id] = entries.size();
    entries.push_back(std::move(entry));
  }

  // 5. Link desktop-backed entries to owning system/profile packages strictly
  // via documented PackageOwnershipLookup or exact Nix store-path prefix.
  for (auto& entry : entries) {
    const InstallationSource src =
        entry.app.primary_provenance.installation_source;
    if (src == InstallationSource::kFlatpak ||
        src == InstallationSource::kSnap ||
        src == InstallationSource::kAppImage) {
      continue;
    }

    for (const auto& desktop : entry.app.desktop_entries) {
      if (!desktop.desktop_file_path.has_value() ||
          desktop.desktop_file_path.value().empty()) {
        continue;
      }

      // 5a. Authoritative Nix store-path ownership (O(1) hash lookup).
      const std::string store_root =
          extract_nix_store_root(desktop.desktop_file_path.value());
      if (!store_root.empty()) {
        const auto nix_it = nix_by_store_path.find(store_root);
        if (nix_it != nix_by_store_path.end()) {
          if (nix_it->second.size() == 1) {
            enrich_from_package(&entry, inventory.packages[nix_it->second[0]]);
            continue;
          }
          if (nix_it->second.size() > 1) {
            ResolutionAmbiguity amb;
            amb.reason =
                "ambiguous Nix profile ownership for desktop entry " +
                desktop.desktop_file_id;
            for (size_t pidx : nix_it->second) {
              amb.candidate_ids.push_back(
                  inventory.packages[pidx].package_identity_id);
            }
            entry.app.ambiguities.push_back(amb);
            inventory.ambiguities.push_back(std::move(amb));
            continue;
          }
        }
      }

      // 5b. Documented package ownership database lookup seam.
      if (ownership_lookup == nullptr) continue;
      const std::vector<PackageOwnershipEvidence> owners =
          ownership_lookup->lookup_file_owners(
              desktop.desktop_file_path.value());
      if (owners.empty()) continue;

      std::vector<size_t> matched_pkgs;
      std::unordered_set<size_t> seen_pkg_idx;
      for (const auto& owner : owners) {
        if (owner.package_identity_id.has_value() &&
            !owner.package_identity_id.value().empty()) {
          const auto id_it =
              pkg_by_identity.find(owner.package_identity_id.value());
          if (id_it != pkg_by_identity.end()) {
            for (size_t pidx : id_it->second) {
              if (seen_pkg_idx.insert(pidx).second) {
                matched_pkgs.push_back(pidx);
              }
            }
          }
          continue;
        }
        const auto pn_it = pkg_by_provider_name.find(
            owner.package_provider_id + "\n" + owner.package_name);
        if (pn_it == pkg_by_provider_name.end()) continue;
        for (size_t pidx : pn_it->second) {
          const auto& pkg = inventory.packages[pidx];
          if (!is_scope_compatible(owner.scope,
                                   pkg.provenance.installation_scope)) {
            continue;
          }
          if (owner.architecture.has_value() && pkg.architecture.has_value() &&
              owner.architecture.value() != pkg.architecture.value()) {
            continue;
          }
          if (owner.profile_path.has_value() && pkg.profile_path.has_value() &&
              owner.profile_path.value() != pkg.profile_path.value()) {
            continue;
          }
          if (seen_pkg_idx.insert(pidx).second) {
            matched_pkgs.push_back(pidx);
          }
        }
      }

      if (matched_pkgs.size() == 1) {
        enrich_from_package(&entry, inventory.packages[matched_pkgs.front()]);
      } else if (matched_pkgs.size() > 1) {
        ResolutionAmbiguity amb;
        amb.reason = "ambiguous package ownership for desktop entry " +
                     desktop.desktop_file_id;
        for (size_t pidx : matched_pkgs) {
          amb.candidate_ids.push_back(
              inventory.packages[pidx].package_identity_id);
        }
        entry.app.ambiguities.push_back(amb);
        inventory.ambiguities.push_back(std::move(amb));
      }
    }
  }

  // 6. Build indexed lookup maps for AppStream metadata (O(A)).
  std::unordered_map<std::string, std::vector<size_t>> as_by_desktop_id;
  std::unordered_map<std::string, std::vector<size_t>> as_by_flatpak;
  std::unordered_map<std::string, std::vector<size_t>> as_by_snap;
  std::unordered_map<std::string, std::vector<size_t>> as_by_pkgname;

  for (size_t i = 0; i < sorted_as.size(); ++i) {
    const auto& rec = sorted_as[i];
    for (const auto& did : rec.desktop_launchable_ids) {
      if (!did.empty()) as_by_desktop_id[did].push_back(i);
    }
    for (const auto& bundle : rec.bundles) {
      if (bundle.kind == "flatpak") {
        const ParsedFlatpakBundle parsed = parse_flatpak_bundle_id(bundle.id);
        if (!parsed.app_id.empty()) {
          as_by_flatpak[parsed.app_id].push_back(i);
        }
      } else if (bundle.kind == "snap" && !bundle.id.empty()) {
        as_by_snap[bundle.id].push_back(i);
      }
    }
    for (const auto& pname : rec.package_names) {
      if (!pname.empty()) as_by_pkgname[pname].push_back(i);
    }
  }

  std::vector<bool> appstream_matched(sorted_as.size(), false);
  std::unordered_set<std::string> matched_component_ids_on_apps;

  // 7. Enrich ResolvedApplications from strongly matched AppStream records.
  for (auto& entry : entries) {
    std::vector<size_t> candidates;

    // 7a. Desktop-ID launchable index (strongest for desktop-backed apps).
    for (const auto& desktop : entry.app.desktop_entries) {
      const auto it = as_by_desktop_id.find(desktop.desktop_file_id);
      if (it == as_by_desktop_id.end()) continue;
      for (size_t aidx : it->second) {
        if (appstream_compatible_with_app(sorted_as[aidx], entry)) {
          candidates.push_back(aidx);
        }
      }
    }

    // 7b. Bundle ID index for Flatpak / Snap apps when desktop launchable
    // lookup yielded no match.
    if (candidates.empty() && entry.flatpak_app_id.has_value()) {
      const auto it = as_by_flatpak.find(entry.flatpak_app_id.value());
      if (it != as_by_flatpak.end()) {
        for (size_t aidx : it->second) {
          if (appstream_compatible_with_app(sorted_as[aidx], entry)) {
            candidates.push_back(aidx);
          }
        }
      }
    }
    if (candidates.empty() && entry.snap_package_name.has_value()) {
      const auto it = as_by_snap.find(entry.snap_package_name.value());
      if (it != as_by_snap.end()) {
        for (size_t aidx : it->second) {
          if (appstream_compatible_with_app(sorted_as[aidx], entry)) {
            candidates.push_back(aidx);
          }
        }
      }
    }

    std::vector<size_t> selected;
    std::vector<std::string> ambiguous_cids;
    if (select_unambiguous_appstream(candidates, sorted_as, &selected,
                                     &ambiguous_cids)) {
      for (size_t aidx : selected) {
        appstream_matched[aidx] = true;
        matched_component_ids_on_apps.insert(sorted_as[aidx].component_id);
        enrich_from_appstream(&entry, sorted_as[aidx]);

        // If this desktop entry has no package link yet and AppStream provides
        // a single unambiguous native package name matching a unique installed
        // distro package, link them without guessing across multi-package
        // ambiguity!
        if (entry.app.linked_package_identity_ids.empty() &&
            entry.app.primary_provenance.installation_source ==
                InstallationSource::kUnknown &&
            sorted_as[aidx].package_names.size() == 1) {
          const auto pit =
              pkg_by_name.find(sorted_as[aidx].package_names.front());
          if (pit != pkg_by_name.end()) {
            if (pit->second.size() == 1) {
              enrich_from_package(&entry, inventory.packages[pit->second[0]]);
            } else if (pit->second.size() > 1) {
              ResolutionAmbiguity amb;
              amb.reason =
                  "ambiguous installed packages for AppStream pkgname " +
                  sorted_as[aidx].package_names.front();
              for (size_t pidx : pit->second) {
                amb.candidate_ids.push_back(
                    inventory.packages[pidx].package_identity_id);
              }
              entry.app.ambiguities.push_back(amb);
              inventory.ambiguities.push_back(std::move(amb));
            }
          }
        }
      }
    } else if (!ambiguous_cids.empty()) {
      ResolutionAmbiguity amb;
      amb.reason =
          "ambiguous AppStream metadata candidates for " +
          entry.app.resolved_id;
      amb.candidate_ids = std::move(ambiguous_cids);
      entry.app.ambiguities.push_back(amb);
      inventory.ambiguities.push_back(std::move(amb));
    }
  }

  // 8. Correlate installed packages with AppStream metadata by authoritative
  // `package_names` index:
  // - Never classify packages from names or sections alone.
  // - Never promote catalog-only AppStream records without an installed
  //   package.
  // - Promote strongly matched CLI/GUI application components backed by a
  //   verified installed package (supporting multiple components per package),
  //   while keeping services, libraries, runtimes, and insufficiently
  //   identified packages strictly in `inventory.packages`.
  for (size_t pidx = 0; pidx < inventory.packages.size(); ++pidx) {
    auto& pkg = inventory.packages[pidx];
    const auto it = as_by_pkgname.find(pkg.package_name);
    if (it == as_by_pkgname.end()) continue;

    // Collect compatible AppStream records grouped by component_id.
    std::map<std::string, std::vector<size_t>> by_component;
    for (size_t aidx : it->second) {
      const auto& rec = sorted_as[aidx];
      if (!is_scope_compatible(rec.provenance.scope,
                               pkg.provenance.installation_scope)) {
        continue;
      }
      // Reject bundle-only records that don't actually name this package.
      by_component[rec.component_id].push_back(aidx);
    }
    if (by_component.empty()) continue;

    // Classify the installed package ONLY if all matched components agree on a
    // single non-unknown classification (otherwise leave kUnknown).
    ApplicationClassification agreed_class = ApplicationClassification::kUnknown;
    bool classification_conflict = false;
    for (const auto& kv : by_component) {
      for (size_t aidx : kv.second) {
        appstream_matched[aidx] = true;
        const ApplicationClassification c =
            classify_appstream_component_kind(sorted_as[aidx].component_kind);
        if (c == ApplicationClassification::kUnknown) continue;
        if (agreed_class == ApplicationClassification::kUnknown) {
          agreed_class = c;
        } else if (agreed_class != c) {
          classification_conflict = true;
        }
      }
    }
    if (!classification_conflict &&
        agreed_class != ApplicationClassification::kUnknown) {
      pkg.classification = agreed_class;
    }

    // Promote strongly matched verified installed CLI/GUI application
    // components that are not already represented by a desktop-backed entry.
    for (const auto& kv : by_component) {
      const std::string& cid = kv.first;
      if (matched_component_ids_on_apps.count(cid) > 0) {
        continue;
      }

      // Order kMetainfo ahead of kCatalog deterministically.
      std::vector<size_t> ordered = kv.second;
      std::sort(ordered.begin(), ordered.end(), [&](size_t a, size_t b) {
        const int ra =
            sorted_as[a].provenance.origin_kind ==
                    AppStreamMetadataOriginKind::kMetainfo
                ? 0
                : 1;
        const int rb =
            sorted_as[b].provenance.origin_kind ==
                    AppStreamMetadataOriginKind::kMetainfo
                ? 0
                : 1;
        return ra != rb ? ra < rb : a < b;
      });

      const ApplicationClassification comp_class =
          classify_appstream_component_kind(
              sorted_as[ordered.front()].component_kind);
      if (comp_class != ApplicationClassification::kCliApplication &&
          comp_class != ApplicationClassification::kGuiApplication) {
        // Services, libraries, runtimes, and generic/unknown components stay
        // strictly in `inventory.packages`.
        continue;
      }

      // Build a deterministic component-qualified resolved_id anchored to the
      // package's stable identity so multiple components belonging to one
      // package (or across separate profiles/arches) stay distinct and stable
      // across upgrades.
      const std::string comp_resolved_id = build_application_id(
          pkg.provenance.provider_id, pkg.package_name,
          pkg.provenance.installation_scope,
          OptionalString::of(pkg.package_identity_id + "#" + cid));
      if (entry_by_resolved_id.count(comp_resolved_id) > 0) continue;

      MutableResolvedEntry promoted;
      promoted.app.resolved_id = comp_resolved_id;
      promoted.app.primary_provenance = pkg.provenance;
      enrich_from_package(&promoted, pkg);
      for (size_t aidx : ordered) {
        enrich_from_appstream(&promoted, sorted_as[aidx]);
      }
      entry_by_resolved_id[comp_resolved_id] = entries.size();
      entries.push_back(std::move(promoted));
    }
  }

  // 9. Finalize classification from authoritative desktop-entry evidence ONLY
  // when AppStream did not already establish a classification. Never guess
  // from names or sections!
  inventory.applications.reserve(entries.size());
  for (auto& entry : entries) {
    if (entry.app.classification == ApplicationClassification::kUnknown &&
        !entry.app.desktop_entries.empty()) {
      if (entry.app.capabilities.requires_terminal ==
          CapabilityState::kSupported) {
        entry.app.classification = ApplicationClassification::kCliApplication;
      } else if (entry.app.capabilities.requires_terminal ==
                 CapabilityState::kUnsupported) {
        entry.app.classification = ApplicationClassification::kGuiApplication;
      }
    }
    std::sort(entry.app.original_record_ids.begin(),
              entry.app.original_record_ids.end());
    std::sort(entry.app.linked_package_identity_ids.begin(),
              entry.app.linked_package_identity_ids.end());
    std::sort(entry.app.linked_appstream_component_ids.begin(),
              entry.app.linked_appstream_component_ids.end());
    inventory.applications.push_back(std::move(entry.app));
  }

  // 10. Deterministic final ordering independent of provider execution order.
  std::sort(inventory.applications.begin(), inventory.applications.end(),
            [](const ResolvedApplication& a, const ResolvedApplication& b) {
              return a.resolved_id < b.resolved_id;
            });

  // 11. Preserve all unmatched / catalog-only AppStream records strictly
  // outside `inventory.applications`.
  for (size_t i = 0; i < sorted_as.size(); ++i) {
    if (!appstream_matched[i]) {
      inventory.unmatched_appstream_records.push_back(sorted_as[i]);
    }
  }

  return inventory;
}

}  // namespace flutter_device_apps_linux
