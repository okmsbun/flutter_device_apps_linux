#include "libappstream_backend.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <gmodule.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace flutter_device_apps_linux {
namespace {

// Matches AsFormatStyle values across AppStream 0.10..1.x:
// AS_FORMAT_STYLE_METAINFO == 1, AS_FORMAT_STYLE_CATALOG/COLLECTION == 2.
constexpr int kAsFormatStyleMetainfo = 1;
constexpr int kAsFormatStyleCatalog = 2;

// Matches AsFormatKind fallback when as_format_kind_from_string is unavailable:
// AS_FORMAT_KIND_XML == 1 across AppStream 0.10..1.x.
constexpr int kAsFormatKindXmlFallback = 1;

// AS_MARKUP_CONVERT_FORMAT_SIMPLE == 0 in AppStream 1.0+.
constexpr int kAsMarkupConvertFormatSimple = 0;

// Safety bound on direct metainfo XML files parsed per explicit directory.
constexpr size_t kMaxMetainfoFilesPerDir = 4096;

struct GObjectDeleter {
  void operator()(gpointer object) const {
    if (object != nullptr) {
      g_object_unref(object);
    }
  }
};

struct GPtrArrayDeleter {
  void operator()(GPtrArray* array) const {
    if (array != nullptr) {
      g_ptr_array_unref(array);
    }
  }
};

struct GDirDeleter {
  void operator()(GDir* dir) const {
    if (dir != nullptr) {
      g_dir_close(dir);
    }
  }
};

using ScopedGObject = std::unique_ptr<void, GObjectDeleter>;
using ScopedGPtrArray = std::unique_ptr<GPtrArray, GPtrArrayDeleter>;
using ScopedGDir = std::unique_ptr<GDir, GDirDeleter>;

enum class AppStreamAbiVersion {
  kNone,
  kAppStream1_So5,
  kAppStream0_So4,
};

struct LibAppStreamSymbols {
  // Core AsPool symbols common across verified ABIs.
  gpointer (*pool_new)() = nullptr;
  gboolean (*pool_load)(gpointer, GCancellable*, GError**) = nullptr;
  gpointer (*pool_get_components)(gpointer) = nullptr;
  void (*pool_set_flags)(gpointer, guint) = nullptr;
  void (*pool_set_locale)(gpointer, const gchar*) = nullptr;
  void (*pool_add_extra_data_location)(gpointer, const gchar*, int) = nullptr;
  void (*pool_set_load_std_data_locations)(gpointer, gboolean) = nullptr;

  // Official AsMetadata API symbols for isolated metainfo XML parsing across
  // both libappstream.so.4 (0.x) and libappstream.so.5 (1.0/1.1+).
  gpointer (*metadata_new)() = nullptr;
  void (*metadata_set_locale)(gpointer, const gchar*) = nullptr;
  void (*metadata_set_format_style)(gpointer, int) = nullptr;
  int (*format_kind_from_string)(const gchar*) = nullptr;
  gboolean (*metadata_parse_file)(gpointer, GFile*, int, GError**) = nullptr;
  gpointer (*metadata_get_component)(gpointer) = nullptr;

  // AppStream 1.0 (libappstream.so.5) container/release symbols.
  GPtrArray* (*component_box_as_array)(gpointer) = nullptr;
  gpointer (*component_get_releases_plain)(gpointer) = nullptr;
  GPtrArray* (*release_list_get_entries)(gpointer) = nullptr;
  gchar* (*markup_convert)(const gchar*, int, GError**) = nullptr;

  // AppStream 0.x (libappstream.so.4) release/markup symbols.
  GPtrArray* (*component_get_releases_v0)(gpointer) = nullptr;
  gchar* (*markup_convert_simple)(const gchar*, GError**) = nullptr;

  // Component metadata getters.
  const gchar* (*component_get_id)(gpointer) = nullptr;
  int (*component_get_kind)(gpointer) = nullptr;
  const gchar* (*component_kind_to_string)(int) = nullptr;
  const gchar* (*component_get_name)(gpointer) = nullptr;
  const gchar* (*component_get_summary)(gpointer) = nullptr;
  const gchar* (*component_get_description)(gpointer) = nullptr;
  const gchar* (*component_get_project_license)(gpointer) = nullptr;
  const gchar* (*component_get_url)(gpointer, int) = nullptr;
  int (*url_kind_from_string)(const gchar*) = nullptr;
  gchar** (*component_get_pkgnames)(gpointer) = nullptr;
  GPtrArray* (*component_get_categories)(gpointer) = nullptr;
  GPtrArray* (*component_get_bundles)(gpointer) = nullptr;
  GPtrArray* (*component_get_launchables)(gpointer) = nullptr;
  GPtrArray* (*component_get_icons)(gpointer) = nullptr;
  const gchar* (*component_get_origin)(gpointer) = nullptr;
  const gchar* (*component_get_data_id)(gpointer) = nullptr;
  int (*component_get_scope)(gpointer) = nullptr;
  const gchar* (*component_scope_to_string)(int) = nullptr;

  // Sub-object getters.
  int (*bundle_get_kind)(gpointer) = nullptr;
  const gchar* (*bundle_kind_to_string)(int) = nullptr;
  const gchar* (*bundle_get_id)(gpointer) = nullptr;

  int (*launchable_get_kind)(gpointer) = nullptr;
  const gchar* (*launchable_kind_to_string)(int) = nullptr;
  GPtrArray* (*launchable_get_entries)(gpointer) = nullptr;

  int (*icon_get_kind)(gpointer) = nullptr;
  const gchar* (*icon_kind_to_string)(int) = nullptr;
  const gchar* (*icon_get_name)(gpointer) = nullptr;
  const gchar* (*icon_get_filename)(gpointer) = nullptr;
  const gchar* (*icon_get_url)(gpointer) = nullptr;

  const gchar* (*release_get_version)(gpointer) = nullptr;
  const gchar* (*release_get_date)(gpointer) = nullptr;
  guint64 (*release_get_timestamp)(gpointer) = nullptr;
  int (*release_get_urgency)(gpointer) = nullptr;
  const gchar* (*urgency_kind_to_string)(int) = nullptr;
};

template <typename Fn>
bool load_symbol(GModule* module, const char* name, Fn* out) {
  static_assert(sizeof(Fn) == sizeof(gpointer),
                "POSIX/GLib function pointer size must match gpointer");
  gpointer symbol = nullptr;
  if (!g_module_symbol(module, name, &symbol) || symbol == nullptr) {
    *out = nullptr;
    return false;
  }
  std::memcpy(out, &symbol, sizeof(*out));
  return true;
}

bool contains_string(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

bool has_suffix(const std::string& value, const char* suffix) {
  const size_t suffix_len = std::strlen(suffix);
  return value.size() >= suffix_len &&
         value.compare(value.size() - suffix_len, suffix_len, suffix) == 0;
}

bool is_metainfo_xml_filename(const std::string& filename) {
  return has_suffix(filename, ".metainfo.xml") ||
         has_suffix(filename, ".appdata.xml");
}

InstallationScope parse_scope(const char* scope_str) {
  if (scope_str == nullptr) return InstallationScope::kUnknown;
  if (std::strcmp(scope_str, "system") == 0) return InstallationScope::kSystem;
  if (std::strcmp(scope_str, "user") == 0) return InstallationScope::kUser;
  return InstallationScope::kUnknown;
}

AppStreamMetadataOriginKind infer_origin_kind(
    const OptionalString& origin, const OptionalString& data_id,
    const LibAppStreamConfig& config,
    AppStreamMetadataOriginKind forced_origin_kind) {
  if (forced_origin_kind != AppStreamMetadataOriginKind::kUnknown) {
    return forced_origin_kind;
  }
  if (!config.load_standard_locations && config.extra_locations.size() == 1) {
    return config.extra_locations.front().origin_kind;
  }
  if (origin.has_value() && !origin.value().empty()) {
    if (origin.value() == "metainfo" || origin.value() == "desktop-entry") {
      return AppStreamMetadataOriginKind::kMetainfo;
    }
    return AppStreamMetadataOriginKind::kCatalog;
  }
  if (data_id.has_value() &&
      data_id.value().find("/metainfo/") != std::string::npos) {
    return AppStreamMetadataOriginKind::kMetainfo;
  }
  return AppStreamMetadataOriginKind::kUnknown;
}

class LibAppStreamBackend final : public AppStreamBackend {
 public:
  explicit LibAppStreamBackend(LibAppStreamConfig config)
      : config_(std::move(config)) {}

  bool is_available() const override {
    ensure_initialized();
    return abi_ != AppStreamAbiVersion::kNone;
  }

  std::string abi_description() const override {
    ensure_initialized();
    switch (abi_) {
      case AppStreamAbiVersion::kAppStream1_So5:
        return "libappstream.so.5 (AppStream 1.0 ABI)";
      case AppStreamAbiVersion::kAppStream0_So4:
        return "libappstream.so.4 (AppStream 0.x ABI)";
      case AppStreamAbiVersion::kNone:
        return "unavailable";
    }
    return "unavailable";
  }

  AppStreamBackendResult load_local_metadata() override {
    ensure_initialized();
    AppStreamBackendResult result;
    if (abi_ == AppStreamAbiVersion::kNone) {
      result.ok = false;
      result.error_kind = init_error_kind_;
      result.error_message = init_error_message_;
      return result;
    }

    const int homepage_url_kind =
        symbols_.url_kind_from_string != nullptr
            ? symbols_.url_kind_from_string("homepage")
            : -1;

    // When load_standard_locations is false, AppStream 1.1+ clears
    // AS_POOL_FLAG_LOAD_OS_METAINFO inside as_pool_set_load_std_data_locations
    // and skips metainfo extra locations in as_pool_load(). Enabling that flag
    // on AsPool would also scan host OS metainfo directories. Instead, parse
    // explicit kMetainfo directories directly through the official AsMetadata
    // API, which is completely isolated from host metadata and ABI-compatible
    // across both libappstream.so.4 and libappstream.so.5.
    std::vector<AppStreamDataLocation> pool_extra_locations;
    std::vector<AppStreamDataLocation> isolated_metainfo_locations;
    const bool use_isolated_metadata =
        !config_.load_standard_locations &&
        symbols_.metadata_new != nullptr &&
        symbols_.metadata_parse_file != nullptr &&
        symbols_.metadata_get_component != nullptr;

    for (const auto& loc : config_.extra_locations) {
      if (loc.path.empty()) continue;
      if (use_isolated_metadata &&
          loc.origin_kind == AppStreamMetadataOriginKind::kMetainfo) {
        isolated_metainfo_locations.push_back(loc);
      } else {
        pool_extra_locations.push_back(loc);
      }
    }

    if (!isolated_metainfo_locations.empty()) {
      load_isolated_metainfo_locations(isolated_metainfo_locations,
                                       homepage_url_kind, &result.records);
    }

    const bool need_pool =
        config_.load_standard_locations || !pool_extra_locations.empty();
    if (!need_pool) {
      result.ok = true;
      return result;
    }

    ScopedGObject pool(symbols_.pool_new());
    if (pool == nullptr) {
      result.ok = false;
      result.error_kind = AppStreamQueryErrorKind::kLoadFailed;
      result.error_message = "as_pool_new() returned null";
      return result;
    }

    if (!config_.locale.empty() && symbols_.pool_set_locale != nullptr) {
      symbols_.pool_set_locale(pool.get(), config_.locale.c_str());
    }

    if (!config_.load_standard_locations) {
      if (symbols_.pool_set_load_std_data_locations != nullptr) {
        symbols_.pool_set_load_std_data_locations(pool.get(), FALSE);
      } else if (symbols_.pool_set_flags != nullptr) {
        symbols_.pool_set_flags(pool.get(), 0);
      }
    }

    if (!pool_extra_locations.empty() &&
        symbols_.pool_add_extra_data_location != nullptr) {
      for (const auto& loc : pool_extra_locations) {
        const int style =
            loc.origin_kind == AppStreamMetadataOriginKind::kCatalog
                ? kAsFormatStyleCatalog
                : kAsFormatStyleMetainfo;
        symbols_.pool_add_extra_data_location(pool.get(), loc.path.c_str(),
                                              style);
      }
    }

    g_autoptr(GError) error = nullptr;
    if (!symbols_.pool_load(pool.get(), nullptr, &error)) {
      result.ok = false;
      result.error_kind = AppStreamQueryErrorKind::kCatalogUnavailable;
      result.error_message =
          error != nullptr && error->message != nullptr
              ? error->message
              : "Unable to load local AppStream metadata pool";
      return result;
    }

    // Explicitly respect the ABI-specific return type and ownership contract of
    // as_pool_get_components():
    // - libappstream.so.5 (AppStream 1.0): returns owned AsComponentBox*
    //   (g_object_unref), borrowed GPtrArray* via as_component_box_as_array().
    // - libappstream.so.4 (AppStream 0.x): returns owned GPtrArray*
    //   (g_ptr_array_unref).
    ScopedGObject component_box;
    ScopedGPtrArray owned_components;
    GPtrArray* components = nullptr;

    if (abi_ == AppStreamAbiVersion::kAppStream1_So5) {
      component_box.reset(symbols_.pool_get_components(pool.get()));
      if (component_box != nullptr) {
        components = symbols_.component_box_as_array(component_box.get());
      }
    } else if (abi_ == AppStreamAbiVersion::kAppStream0_So4) {
      owned_components.reset(
          static_cast<GPtrArray*>(symbols_.pool_get_components(pool.get())));
      components = owned_components.get();
    }

    if (components == nullptr) {
      result.ok = true;
      return result;
    }

    const AppStreamMetadataOriginKind pool_forced_origin =
        (!config_.load_standard_locations && pool_extra_locations.size() == 1)
            ? pool_extra_locations.front().origin_kind
            : AppStreamMetadataOriginKind::kUnknown;

    result.records.reserve(result.records.size() + components->len);
    for (guint i = 0; i < components->len; ++i) {
      gpointer cpt = g_ptr_array_index(components, i);
      AppStreamMetadataRecord record;
      if (extract_component_record(cpt, homepage_url_kind, pool_forced_origin,
                                   &record)) {
        result.records.push_back(std::move(record));
      }
    }

    result.ok = true;
    return result;
  }

 private:
  void load_isolated_metainfo_locations(
      const std::vector<AppStreamDataLocation>& locations,
      int homepage_url_kind,
      std::vector<AppStreamMetadataRecord>* out_records) const {
    const int xml_format_kind =
        symbols_.format_kind_from_string != nullptr
            ? symbols_.format_kind_from_string("xml")
            : kAsFormatKindXmlFallback;
    const int effective_xml_kind =
        xml_format_kind > 0 ? xml_format_kind : kAsFormatKindXmlFallback;

    for (const auto& loc : locations) {
      ScopedGDir dir(g_dir_open(loc.path.c_str(), 0, nullptr));
      if (dir == nullptr) continue;

      std::vector<std::string> files;
      while (const gchar* entry = g_dir_read_name(dir.get())) {
        if (files.size() >= kMaxMetainfoFilesPerDir) break;
        const std::string name(entry);
        if (!is_metainfo_xml_filename(name)) continue;
        g_autofree gchar* full_path =
            g_build_filename(loc.path.c_str(), entry, nullptr);
        if (full_path != nullptr &&
            g_file_test(full_path, G_FILE_TEST_IS_REGULAR)) {
          files.emplace_back(full_path);
        }
      }
      std::sort(files.begin(), files.end());

      for (const auto& file_path : files) {
        ScopedGObject mdata(symbols_.metadata_new());
        if (mdata == nullptr) continue;
        if (symbols_.metadata_set_format_style != nullptr) {
          symbols_.metadata_set_format_style(mdata.get(),
                                             kAsFormatStyleMetainfo);
        }
        if (!config_.locale.empty() &&
            symbols_.metadata_set_locale != nullptr) {
          symbols_.metadata_set_locale(mdata.get(), config_.locale.c_str());
        }

        ScopedGObject gfile(g_file_new_for_path(file_path.c_str()));
        if (gfile == nullptr) continue;

        g_autoptr(GError) parse_error = nullptr;
        if (!symbols_.metadata_parse_file(mdata.get(),
                                          static_cast<GFile*>(gfile.get()),
                                          effective_xml_kind, &parse_error)) {
          // Skip malformed or unreadable XML files gracefully without aborting
          // valid metainfo files in the same directory.
          continue;
        }

        // as_metadata_get_component() returns a borrowed AsComponent*
        // (transfer none) owned by mdata in both libappstream.so.4 and .so.5.
        gpointer cpt = symbols_.metadata_get_component(mdata.get());
        AppStreamMetadataRecord record;
        if (extract_component_record(
                cpt, homepage_url_kind, AppStreamMetadataOriginKind::kMetainfo,
                &record)) {
          if (!record.provenance.origin.has_value()) {
            record.provenance.origin = OptionalString::of("metainfo");
          }
          out_records->push_back(std::move(record));
        }
      }
    }
  }

  bool extract_component_record(gpointer cpt, int homepage_url_kind,
                                AppStreamMetadataOriginKind forced_origin_kind,
                                AppStreamMetadataRecord* out_record) const {
    if (cpt == nullptr || out_record == nullptr) return false;

    const gchar* raw_id = symbols_.component_get_id(cpt);
    if (raw_id == nullptr || *raw_id == '\0') return false;

    AppStreamMetadataRecord record;
    record.component_id = raw_id;

    if (symbols_.component_get_kind != nullptr &&
        symbols_.component_kind_to_string != nullptr) {
      const gchar* kind_str =
          symbols_.component_kind_to_string(symbols_.component_get_kind(cpt));
      record.component_kind =
          kind_str != nullptr && *kind_str != '\0' ? kind_str : "unknown";
    } else {
      record.component_kind = "unknown";
    }

    if (symbols_.component_get_name != nullptr) {
      const gchar* name = symbols_.component_get_name(cpt);
      if (name != nullptr && *name != '\0') {
        record.name = OptionalString::of(name);
      }
    }
    if (symbols_.component_get_summary != nullptr) {
      const gchar* summary = symbols_.component_get_summary(cpt);
      if (summary != nullptr && *summary != '\0') {
        record.summary = OptionalString::of(summary);
      }
    }
    if (symbols_.component_get_description != nullptr) {
      const gchar* raw_desc = symbols_.component_get_description(cpt);
      if (raw_desc != nullptr && *raw_desc != '\0') {
        record.description = convert_description(raw_desc);
      }
    }
    if (symbols_.component_get_project_license != nullptr) {
      const gchar* license = symbols_.component_get_project_license(cpt);
      if (license != nullptr && *license != '\0') {
        record.project_license = OptionalString::of(license);
      }
    }
    if (homepage_url_kind >= 0 && symbols_.component_get_url != nullptr) {
      const gchar* url = symbols_.component_get_url(cpt, homepage_url_kind);
      if (url != nullptr && *url != '\0') {
        record.homepage = OptionalString::of(url);
      }
    }

    if (symbols_.component_get_pkgnames != nullptr) {
      gchar** pkgnames = symbols_.component_get_pkgnames(cpt);
      for (gchar** p = pkgnames; p != nullptr && *p != nullptr; ++p) {
        if (**p != '\0') {
          record.package_names.emplace_back(*p);
        }
      }
    }

    if (symbols_.component_get_categories != nullptr) {
      GPtrArray* cats = symbols_.component_get_categories(cpt);
      for (guint j = 0; cats != nullptr && j < cats->len; ++j) {
        const gchar* cat =
            static_cast<const gchar*>(g_ptr_array_index(cats, j));
        if (cat != nullptr && *cat != '\0') {
          record.categories.emplace_back(cat);
        }
      }
    }

    if (symbols_.component_get_bundles != nullptr &&
        symbols_.bundle_get_kind != nullptr &&
        symbols_.bundle_kind_to_string != nullptr &&
        symbols_.bundle_get_id != nullptr) {
      GPtrArray* bundles = symbols_.component_get_bundles(cpt);
      for (guint j = 0; bundles != nullptr && j < bundles->len; ++j) {
        gpointer bundle = g_ptr_array_index(bundles, j);
        if (bundle == nullptr) continue;
        const gchar* bkind =
            symbols_.bundle_kind_to_string(symbols_.bundle_get_kind(bundle));
        const gchar* bid = symbols_.bundle_get_id(bundle);
        if (bid != nullptr && *bid != '\0') {
          AppStreamBundleReference ref;
          ref.kind = bkind != nullptr ? bkind : "unknown";
          ref.id = bid;
          record.bundles.push_back(std::move(ref));
        }
      }
    }

    if (symbols_.component_get_launchables != nullptr &&
        symbols_.launchable_get_kind != nullptr &&
        symbols_.launchable_kind_to_string != nullptr &&
        symbols_.launchable_get_entries != nullptr) {
      GPtrArray* launchables = symbols_.component_get_launchables(cpt);
      for (guint j = 0; launchables != nullptr && j < launchables->len; ++j) {
        gpointer launchable = g_ptr_array_index(launchables, j);
        if (launchable == nullptr) continue;
        const gchar* lkind = symbols_.launchable_kind_to_string(
            symbols_.launchable_get_kind(launchable));
        if (lkind == nullptr || std::strcmp(lkind, "desktop-id") != 0) {
          continue;
        }
        GPtrArray* entries = symbols_.launchable_get_entries(launchable);
        for (guint k = 0; entries != nullptr && k < entries->len; ++k) {
          const gchar* entry =
              static_cast<const gchar*>(g_ptr_array_index(entries, k));
          if (entry != nullptr && *entry != '\0') {
            record.desktop_launchable_ids.emplace_back(entry);
          }
        }
      }
    }

    if (symbols_.component_get_icons != nullptr &&
        symbols_.icon_get_kind != nullptr &&
        symbols_.icon_kind_to_string != nullptr) {
      GPtrArray* icons = symbols_.component_get_icons(cpt);
      for (guint j = 0; icons != nullptr && j < icons->len; ++j) {
        gpointer icon = g_ptr_array_index(icons, j);
        if (icon == nullptr) continue;
        const gchar* ikind =
            symbols_.icon_kind_to_string(symbols_.icon_get_kind(icon));
        const gchar* val = nullptr;
        if (ikind != nullptr && std::strcmp(ikind, "remote") == 0) {
          // Preserve remote URL strictly as an opaque reference; never fetch.
          if (symbols_.icon_get_url != nullptr) {
            val = symbols_.icon_get_url(icon);
          }
        } else {
          if (symbols_.icon_get_filename != nullptr) {
            val = symbols_.icon_get_filename(icon);
          }
          if ((val == nullptr || *val == '\0') &&
              symbols_.icon_get_name != nullptr) {
            val = symbols_.icon_get_name(icon);
          }
        }
        if (val != nullptr && *val != '\0') {
          AppStreamIconReference iref;
          iref.kind = ikind != nullptr ? ikind : "unknown";
          iref.value = val;
          record.icons.push_back(std::move(iref));
        }
      }
    }

    extract_releases(cpt, &record.releases);

    if (symbols_.component_get_origin != nullptr) {
      const gchar* origin = symbols_.component_get_origin(cpt);
      if (origin != nullptr && *origin != '\0') {
        record.provenance.origin = OptionalString::of(origin);
      }
    }
    if (symbols_.component_get_data_id != nullptr) {
      const gchar* data_id = symbols_.component_get_data_id(cpt);
      if (data_id != nullptr && *data_id != '\0') {
        record.provenance.data_id = OptionalString::of(data_id);
      }
    }
    if (symbols_.component_get_scope != nullptr &&
        symbols_.component_scope_to_string != nullptr) {
      record.provenance.scope = parse_scope(
          symbols_.component_scope_to_string(symbols_.component_get_scope(cpt)));
    }
    record.provenance.origin_kind =
        infer_origin_kind(record.provenance.origin, record.provenance.data_id,
                          config_, forced_origin_kind);
    record.provenance.provider_id = "appstream";
    record.provenance.is_installation_proof = false;

    *out_record = std::move(record);
    return true;
  }

  OptionalString convert_description(const gchar* raw_desc) const {
    if (abi_ == AppStreamAbiVersion::kAppStream1_So5 &&
        symbols_.markup_convert != nullptr) {
      g_autofree gchar* plain = symbols_.markup_convert(
          raw_desc, kAsMarkupConvertFormatSimple, nullptr);
      if (plain != nullptr && *plain != '\0') {
        return OptionalString::of(plain);
      }
    } else if (abi_ == AppStreamAbiVersion::kAppStream0_So4 &&
               symbols_.markup_convert_simple != nullptr) {
      g_autofree gchar* plain =
          symbols_.markup_convert_simple(raw_desc, nullptr);
      if (plain != nullptr && *plain != '\0') {
        return OptionalString::of(plain);
      }
    }
    return OptionalString::of(raw_desc);
  }

  void extract_releases(gpointer cpt,
                        std::vector<AppStreamReleaseMetadata>* out) const {
    GPtrArray* releases = nullptr;
    if (abi_ == AppStreamAbiVersion::kAppStream1_So5 &&
        symbols_.component_get_releases_plain != nullptr &&
        symbols_.release_list_get_entries != nullptr) {
      gpointer release_list = symbols_.component_get_releases_plain(cpt);
      if (release_list != nullptr) {
        releases = symbols_.release_list_get_entries(release_list);
      }
    } else if (abi_ == AppStreamAbiVersion::kAppStream0_So4 &&
               symbols_.component_get_releases_v0 != nullptr) {
      releases = symbols_.component_get_releases_v0(cpt);
    }

    for (guint j = 0; releases != nullptr && j < releases->len; ++j) {
      gpointer rel = g_ptr_array_index(releases, j);
      if (rel == nullptr) continue;

      AppStreamReleaseMetadata item;
      if (symbols_.release_get_version != nullptr) {
        const gchar* ver = symbols_.release_get_version(rel);
        if (ver != nullptr && *ver != '\0') {
          item.version = OptionalString::of(ver);
        }
      }
      if (symbols_.release_get_date != nullptr) {
        const gchar* date = symbols_.release_get_date(rel);
        if (date != nullptr && *date != '\0') {
          item.date = OptionalString::of(date);
        }
      }
      if (symbols_.release_get_timestamp != nullptr) {
        const guint64 ts = symbols_.release_get_timestamp(rel);
        if (ts > 0) {
          item.timestamp = static_cast<int64_t>(ts);
          item.has_timestamp = true;
        }
      }
      if (symbols_.release_get_urgency != nullptr &&
          symbols_.urgency_kind_to_string != nullptr) {
        const gchar* urgency = symbols_.urgency_kind_to_string(
            symbols_.release_get_urgency(rel));
        if (urgency != nullptr && *urgency != '\0' &&
            std::strcmp(urgency, "unknown") != 0) {
          item.urgency = OptionalString::of(urgency);
        }
      }
      if (item.version.has_value() || item.date.has_value() ||
          item.has_timestamp) {
        out->push_back(std::move(item));
      }
    }
  }

  bool try_load_candidate(const std::string& library_name) const {
    if (library_name.empty()) return false;
    GModule* module = g_module_open(
        library_name.c_str(),
        static_cast<GModuleFlags>(G_MODULE_BIND_LOCAL | G_MODULE_BIND_LAZY));
    if (module == nullptr) {
      return false;
    }

    LibAppStreamSymbols syms;
    const bool common_ok =
        load_symbol(module, "as_pool_new", &syms.pool_new) &&
        load_symbol(module, "as_pool_load", &syms.pool_load) &&
        load_symbol(module, "as_pool_get_components",
                    &syms.pool_get_components) &&
        load_symbol(module, "as_component_get_id", &syms.component_get_id);

    if (!common_ok) {
      g_module_close(module);
      init_error_kind_ = AppStreamQueryErrorKind::kUnsupportedAbi;
      init_error_message_ =
          "Loaded " + library_name + " is missing required AppStream symbols";
      return false;
    }

    const bool has_v5_box =
        load_symbol(module, "as_component_box_as_array",
                    &syms.component_box_as_array);
    const bool expects_v5 = contains_string(library_name, ".so.5");
    const bool expects_v4 = contains_string(library_name, ".so.4");

    AppStreamAbiVersion detected = AppStreamAbiVersion::kNone;
    if (has_v5_box && !expects_v4) {
      detected = AppStreamAbiVersion::kAppStream1_So5;
      load_symbol(module, "as_component_get_releases_plain",
                  &syms.component_get_releases_plain);
      load_symbol(module, "as_release_list_get_entries",
                  &syms.release_list_get_entries);
      load_symbol(module, "as_markup_convert", &syms.markup_convert);
    } else if (!has_v5_box && !expects_v5) {
      // Only accept 0.x ABI when libappstream.so.4 (or an explicitly tested
      // 0.x shared object) does NOT expose AppStream 1.0 AsComponentBox ABI.
      detected = AppStreamAbiVersion::kAppStream0_So4;
      load_symbol(module, "as_component_get_releases",
                  &syms.component_get_releases_v0);
      load_symbol(module, "as_markup_convert_simple",
                  &syms.markup_convert_simple);
    } else {
      g_module_close(module);
      init_error_kind_ = AppStreamQueryErrorKind::kUnsupportedAbi;
      init_error_message_ =
          "SONAME and exported ABI mismatch for " + library_name;
      return false;
    }

    // Optional pool configuration, isolated AsMetadata, and component getters.
    load_symbol(module, "as_pool_set_flags", &syms.pool_set_flags);
    load_symbol(module, "as_pool_set_locale", &syms.pool_set_locale);
    load_symbol(module, "as_pool_add_extra_data_location",
                &syms.pool_add_extra_data_location);
    load_symbol(module, "as_pool_set_load_std_data_locations",
                &syms.pool_set_load_std_data_locations);

    load_symbol(module, "as_metadata_new", &syms.metadata_new);
    load_symbol(module, "as_metadata_set_locale", &syms.metadata_set_locale);
    load_symbol(module, "as_metadata_set_format_style",
                &syms.metadata_set_format_style);
    load_symbol(module, "as_format_kind_from_string",
                &syms.format_kind_from_string);
    load_symbol(module, "as_metadata_parse_file", &syms.metadata_parse_file);
    load_symbol(module, "as_metadata_get_component",
                &syms.metadata_get_component);

    load_symbol(module, "as_component_get_kind", &syms.component_get_kind);
    load_symbol(module, "as_component_kind_to_string",
                &syms.component_kind_to_string);
    load_symbol(module, "as_component_get_name", &syms.component_get_name);
    load_symbol(module, "as_component_get_summary",
                &syms.component_get_summary);
    load_symbol(module, "as_component_get_description",
                &syms.component_get_description);
    load_symbol(module, "as_component_get_project_license",
                &syms.component_get_project_license);
    load_symbol(module, "as_component_get_url", &syms.component_get_url);
    load_symbol(module, "as_url_kind_from_string", &syms.url_kind_from_string);
    load_symbol(module, "as_component_get_pkgnames",
                &syms.component_get_pkgnames);
    load_symbol(module, "as_component_get_categories",
                &syms.component_get_categories);
    load_symbol(module, "as_component_get_bundles",
                &syms.component_get_bundles);
    load_symbol(module, "as_component_get_launchables",
                &syms.component_get_launchables);
    load_symbol(module, "as_component_get_icons", &syms.component_get_icons);
    load_symbol(module, "as_component_get_origin", &syms.component_get_origin);
    load_symbol(module, "as_component_get_data_id",
                &syms.component_get_data_id);
    load_symbol(module, "as_component_get_scope", &syms.component_get_scope);
    load_symbol(module, "as_component_scope_to_string",
                &syms.component_scope_to_string);

    load_symbol(module, "as_bundle_get_kind", &syms.bundle_get_kind);
    load_symbol(module, "as_bundle_kind_to_string",
                &syms.bundle_kind_to_string);
    load_symbol(module, "as_bundle_get_id", &syms.bundle_get_id);

    load_symbol(module, "as_launchable_get_kind", &syms.launchable_get_kind);
    load_symbol(module, "as_launchable_kind_to_string",
                &syms.launchable_kind_to_string);
    load_symbol(module, "as_launchable_get_entries",
                &syms.launchable_get_entries);

    load_symbol(module, "as_icon_get_kind", &syms.icon_get_kind);
    load_symbol(module, "as_icon_kind_to_string", &syms.icon_kind_to_string);
    load_symbol(module, "as_icon_get_name", &syms.icon_get_name);
    load_symbol(module, "as_icon_get_filename", &syms.icon_get_filename);
    load_symbol(module, "as_icon_get_url", &syms.icon_get_url);

    load_symbol(module, "as_release_get_version", &syms.release_get_version);
    load_symbol(module, "as_release_get_date", &syms.release_get_date);
    load_symbol(module, "as_release_get_timestamp",
                &syms.release_get_timestamp);
    load_symbol(module, "as_release_get_urgency", &syms.release_get_urgency);
    load_symbol(module, "as_urgency_kind_to_string",
                &syms.urgency_kind_to_string);

    // libappstream registers static GTypes in GLib's type registry; keep the
    // verified module resident.
    g_module_make_resident(module);
    symbols_ = syms;
    abi_ = detected;
    init_error_kind_ = AppStreamQueryErrorKind::kNone;
    init_error_message_.clear();
    return true;
  }

  void ensure_initialized() const {
    std::call_once(init_once_, [this]() {
      if (!g_module_supported()) {
        init_error_kind_ = AppStreamQueryErrorKind::kLibraryUnavailable;
        init_error_message_ = "GModule dynamic loading is not supported";
        return;
      }
      init_error_kind_ = AppStreamQueryErrorKind::kLibraryUnavailable;
      init_error_message_ = "No supported libappstream shared library found";
      for (const auto& candidate : config_.library_candidates) {
        if (try_load_candidate(candidate)) {
          return;
        }
      }
    });
  }

  LibAppStreamConfig config_;
  mutable std::once_flag init_once_;
  mutable AppStreamAbiVersion abi_ = AppStreamAbiVersion::kNone;
  mutable LibAppStreamSymbols symbols_;
  mutable AppStreamQueryErrorKind init_error_kind_ =
      AppStreamQueryErrorKind::kLibraryUnavailable;
  mutable std::string init_error_message_;
};

}  // namespace

std::unique_ptr<AppStreamBackend> create_libappstream_backend(
    LibAppStreamConfig config) {
  return std::unique_ptr<AppStreamBackend>(
      new LibAppStreamBackend(std::move(config)));
}

}  // namespace flutter_device_apps_linux
