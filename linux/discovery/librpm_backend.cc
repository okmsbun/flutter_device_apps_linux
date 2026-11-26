#include "librpm_backend.h"

#include <fcntl.h>
#include <gmodule.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace flutter_device_apps_linux {
namespace {

// RPMDBI_PACKAGES == 0 across RPM 4.x and 6.x (iterates all installed headers).
constexpr int kRpmDbiPackages = 0;
constexpr int kRpmTagNotFound = -1;

struct LibRpmSymbols {
  int (*read_config_files)(const char*, const char*) = nullptr;
  void* (*ts_create)() = nullptr;
  void* (*ts_free)(void*) = nullptr;
  int (*ts_set_root_dir)(void*, const char*) = nullptr;
  int (*ts_open_db)(void*, int) = nullptr;
  int (*ts_close_db)(void*) = nullptr;
  void* (*ts_init_iterator)(const void*, int, const void*, size_t) = nullptr;
  void* (*db_next_iterator)(void*) = nullptr;
  void* (*db_free_iterator)(void*) = nullptr;
  const char* (*header_get_string)(void*, int) = nullptr;
  uint64_t (*header_get_number)(void*, int) = nullptr;
  int (*header_is_entry)(void*, int) = nullptr;
  int (*tag_get_value)(const char*) = nullptr;
  // Optional file-ownership symbols (rpmfi API).
  void* (*fi_new)(const void*, void*, int, unsigned int) = nullptr;
  void* (*fi_free)(void*) = nullptr;
  void* (*fi_init)(void*, int) = nullptr;
  int (*fi_next)(void*) = nullptr;
  const char* (*fi_fn)(void*) = nullptr;
};

struct ResolvedRpmTags {
  int name = kRpmTagNotFound;
  int epoch = kRpmTagNotFound;
  int version = kRpmTagNotFound;
  int release = kRpmTagNotFound;
  int arch = kRpmTagNotFound;
  int summary = kRpmTagNotFound;
  int description = kRpmTagNotFound;
  int license = kRpmTagNotFound;
  int url = kRpmTagNotFound;
  int vendor = kRpmTagNotFound;
  int group = kRpmTagNotFound;
  int basenames = kRpmTagNotFound;
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

// Process-global RPM configuration must be initialized at most once and never
// torn down while the process is running to avoid corrupting global RPM state.
bool ensure_rpm_config_initialized(int (*read_config_files)(const char*,
                                                            const char*)) {
  static std::once_flag config_once;
  static bool config_ok = false;
  std::call_once(config_once, [&]() {
    config_ok = (read_config_files(nullptr, nullptr) == 0);
  });
  return config_ok;
}

OptionalString read_optional_string(const LibRpmSymbols& syms, void* header,
                                    int tag) {
  if (tag == kRpmTagNotFound) return OptionalString::none();
  const char* value = syms.header_get_string(header, tag);
  if (value == nullptr || *value == '\0') return OptionalString::none();
  return OptionalString::of(value);
}

class LibRpmBackend final : public RpmBackend {
 public:
  explicit LibRpmBackend(LibRpmConfig config) : config_(std::move(config)) {}

  bool is_available() const override {
    ensure_initialized();
    return available_;
  }

  std::string availability_error() const override {
    ensure_initialized();
    return unavailable_reason_;
  }

  std::string abi_description() const override {
    ensure_initialized();
    return abi_description_;
  }

  RpmBackendQueryResult list_installed_packages() override {
    ensure_initialized();
    RpmBackendQueryResult result;
    if (!available_) {
      result.ok = false;
      result.error_kind = init_error_kind_;
      result.error_message = unavailable_reason_;
      return result;
    }

    if (!ensure_rpm_config_initialized(symbols_.read_config_files)) {
      result.ok = false;
      result.error_kind = RpmQueryErrorKind::kQueryFailed;
      result.error_message = "rpmReadConfigFiles() failed";
      return result;
    }

    struct ScopedRpmTs {
      const LibRpmSymbols* syms = nullptr;
      void* ts = nullptr;
      bool db_open = false;

      ~ScopedRpmTs() {
        if (ts != nullptr && syms != nullptr) {
          if (db_open && syms->ts_close_db != nullptr) {
            syms->ts_close_db(ts);
          }
          syms->ts_free(ts);
        }
      }
    };

    ScopedRpmTs ts_guard{&symbols_, symbols_.ts_create(), false};
    if (ts_guard.ts == nullptr) {
      result.ok = false;
      result.error_kind = RpmQueryErrorKind::kQueryFailed;
      result.error_message = "rpmtsCreate() returned null";
      return result;
    }

    if (!config_.root_dir.empty()) {
      if (symbols_.ts_set_root_dir == nullptr) {
        result.ok = false;
        result.error_kind = RpmQueryErrorKind::kUnsupportedAbi;
        result.error_message = "rpmtsSetRootDir() symbol is unavailable";
        return result;
      }
      if (symbols_.ts_set_root_dir(ts_guard.ts, config_.root_dir.c_str()) !=
          0) {
        result.ok = false;
        result.error_kind = RpmQueryErrorKind::kQueryFailed;
        result.error_message = "rpmtsSetRootDir() failed";
        return result;
      }
    }

    errno = 0;
    const int open_rc = symbols_.ts_open_db(ts_guard.ts, O_RDONLY);
    if (open_rc != 0) {
      const int err = errno;
      result.ok = false;
      if (err == EACCES || err == EPERM) {
        result.error_kind = RpmQueryErrorKind::kPermissionDenied;
        result.error_message = std::strerror(err);
      } else {
        result.error_kind = RpmQueryErrorKind::kDatabaseUnavailable;
        result.error_message =
            err != 0 ? std::strerror(err)
                     : "Unable to open RPM database in read-only mode";
      }
      return result;
    }
    ts_guard.db_open = true;

    struct ScopedRpmIterator {
      const LibRpmSymbols* syms = nullptr;
      void* mi = nullptr;

      ~ScopedRpmIterator() {
        if (mi != nullptr && syms != nullptr) {
          syms->db_free_iterator(mi);
        }
      }
    };

    ScopedRpmIterator iter{
        &symbols_,
        symbols_.ts_init_iterator(ts_guard.ts, kRpmDbiPackages, nullptr, 0)};
    if (iter.mi == nullptr) {
      // An initialized but empty RPM database can return a null iterator.
      result.ok = true;
      return result;
    }

    // Header pointers returned by rpmdbNextIterator() are owned by the
    // iterator and must NOT be freed by the caller.
    while (void* header = symbols_.db_next_iterator(iter.mi)) {
      const char* raw_name = symbols_.header_get_string(header, tags_.name);
      if (raw_name == nullptr || *raw_name == '\0') continue;

      RpmInstalledPackageRecord pkg;
      pkg.name = raw_name;

      if (tags_.epoch != kRpmTagNotFound &&
          symbols_.header_is_entry(header, tags_.epoch) != 0) {
        const uint64_t epoch_num =
            symbols_.header_get_number(header, tags_.epoch);
        pkg.epoch = OptionalString::of(std::to_string(epoch_num));
      }

      pkg.version = read_optional_string(symbols_, header, tags_.version);
      pkg.release = read_optional_string(symbols_, header, tags_.release);
      pkg.architecture = read_optional_string(symbols_, header, tags_.arch);
      pkg.summary = read_optional_string(symbols_, header, tags_.summary);
      pkg.description =
          read_optional_string(symbols_, header, tags_.description);
      pkg.license = read_optional_string(symbols_, header, tags_.license);
      pkg.homepage = read_optional_string(symbols_, header, tags_.url);
      pkg.vendor = read_optional_string(symbols_, header, tags_.vendor);
      pkg.package_group = read_optional_string(symbols_, header, tags_.group);

      result.packages.push_back(std::move(pkg));
    }

    result.ok = true;
    return result;
  }

  RpmFileOwnershipResult find_file_owners(
      const std::vector<std::string>& absolute_paths) override {
    ensure_initialized();
    RpmFileOwnershipResult result;
    if (!available_) {
      result.ok = false;
      result.error_kind = init_error_kind_;
      result.error_message = unavailable_reason_;
      return result;
    }
    if (symbols_.fi_new == nullptr || symbols_.fi_free == nullptr ||
        symbols_.fi_init == nullptr || symbols_.fi_next == nullptr ||
        symbols_.fi_fn == nullptr || tags_.basenames == kRpmTagNotFound) {
      result.ok = false;
      result.error_kind = RpmQueryErrorKind::kUnsupportedAbi;
      result.error_message = "librpm rpmfi file ownership API is unavailable";
      return result;
    }
    if (!ensure_rpm_config_initialized(symbols_.read_config_files)) {
      result.ok = false;
      result.error_kind = RpmQueryErrorKind::kQueryFailed;
      result.error_message = "rpmReadConfigFiles() failed";
      return result;
    }

    void* ts = symbols_.ts_create();
    if (ts == nullptr) {
      result.ok = false;
      result.error_kind = RpmQueryErrorKind::kQueryFailed;
      result.error_message = "rpmtsCreate() returned null";
      return result;
    }
    bool db_open = false;
    auto cleanup = [&]() {
      if (db_open && symbols_.ts_close_db != nullptr) symbols_.ts_close_db(ts);
      symbols_.ts_free(ts);
    };

    if (!config_.root_dir.empty()) {
      if (symbols_.ts_set_root_dir == nullptr ||
          symbols_.ts_set_root_dir(ts, config_.root_dir.c_str()) != 0) {
        result.ok = false;
        result.error_kind = RpmQueryErrorKind::kQueryFailed;
        result.error_message = "rpmtsSetRootDir() failed or unavailable";
        cleanup();
        return result;
      }
    }

    errno = 0;
    if (symbols_.ts_open_db(ts, O_RDONLY) != 0) {
      const int err = errno;
      result.ok = false;
      result.error_kind = (err == EACCES || err == EPERM)
                              ? RpmQueryErrorKind::kPermissionDenied
                              : RpmQueryErrorKind::kDatabaseUnavailable;
      result.error_message = err != 0 ? std::strerror(err)
                                      : "Unable to open RPM database";
      cleanup();
      return result;
    }
    db_open = true;

    for (const auto& target : absolute_paths) {
      const size_t slash = target.rfind('/');
      if (slash == std::string::npos || slash + 1 >= target.size()) continue;
      const std::string basename = target.substr(slash + 1);

      // RPMDBI_BASENAMES == RPMTAG_BASENAMES; the index keys on basenames only,
      // so every candidate is verified against the exact full path via rpmfi.
      void* mi = symbols_.ts_init_iterator(ts, tags_.basenames,
                                           basename.c_str(), 0);
      if (mi == nullptr) continue;
      while (void* header = symbols_.db_next_iterator(mi)) {
        void* fi = symbols_.fi_new(ts, header, tags_.basenames, 0);
        if (fi == nullptr) continue;
        bool matched = false;
        symbols_.fi_init(fi, 0);
        while (symbols_.fi_next(fi) >= 0) {
          const char* fn = symbols_.fi_fn(fi);
          if (fn != nullptr && target == fn) {
            matched = true;
            break;
          }
        }
        symbols_.fi_free(fi);
        if (!matched) continue;

        const char* raw_name = symbols_.header_get_string(header, tags_.name);
        if (raw_name == nullptr || *raw_name == '\0') continue;
        RpmFileOwnerRecord owner;
        owner.path = target;
        owner.package.name = raw_name;
        if (tags_.epoch != kRpmTagNotFound &&
            symbols_.header_is_entry(header, tags_.epoch) != 0) {
          owner.package.epoch = OptionalString::of(std::to_string(
              symbols_.header_get_number(header, tags_.epoch)));
        }
        owner.package.version =
            read_optional_string(symbols_, header, tags_.version);
        owner.package.release =
            read_optional_string(symbols_, header, tags_.release);
        owner.package.architecture =
            read_optional_string(symbols_, header, tags_.arch);
        result.owners.push_back(std::move(owner));
      }
      symbols_.db_free_iterator(mi);
    }

    cleanup();
    result.ok = true;
    return result;
  }

 private:
  bool try_load_candidate(const std::string& library_name) const {
    if (library_name.empty()) return false;
    GModule* module = g_module_open(
        library_name.c_str(),
        static_cast<GModuleFlags>(G_MODULE_BIND_LOCAL | G_MODULE_BIND_LAZY));
    if (module == nullptr) {
      const char* mod_err = g_module_error();
      if (last_load_error_.empty()) {
        last_load_error_ =
            "failed to load " + library_name +
            (mod_err != nullptr && *mod_err != '\0'
                 ? std::string(": ") + mod_err
                 : std::string(" (library not found)"));
      }
      return false;
    }

    LibRpmSymbols syms;
    std::vector<std::string> missing;
    if (!load_symbol(module, "rpmReadConfigFiles", &syms.read_config_files)) {
      missing.emplace_back("rpmReadConfigFiles");
    }
    if (!load_symbol(module, "rpmtsCreate", &syms.ts_create)) {
      missing.emplace_back("rpmtsCreate");
    }
    if (!load_symbol(module, "rpmtsFree", &syms.ts_free)) {
      missing.emplace_back("rpmtsFree");
    }
    if (!load_symbol(module, "rpmtsOpenDB", &syms.ts_open_db)) {
      missing.emplace_back("rpmtsOpenDB");
    }
    if (!load_symbol(module, "rpmtsInitIterator", &syms.ts_init_iterator)) {
      missing.emplace_back("rpmtsInitIterator");
    }
    if (!load_symbol(module, "rpmdbNextIterator", &syms.db_next_iterator)) {
      missing.emplace_back("rpmdbNextIterator");
    }
    if (!load_symbol(module, "rpmdbFreeIterator", &syms.db_free_iterator)) {
      missing.emplace_back("rpmdbFreeIterator");
    }
    if (!load_symbol(module, "headerGetString", &syms.header_get_string)) {
      missing.emplace_back("headerGetString");
    }
    if (!load_symbol(module, "headerGetNumber", &syms.header_get_number)) {
      missing.emplace_back("headerGetNumber");
    }
    if (!load_symbol(module, "headerIsEntry", &syms.header_is_entry)) {
      missing.emplace_back("headerIsEntry");
    }
    if (!load_symbol(module, "rpmTagGetValue", &syms.tag_get_value)) {
      missing.emplace_back("rpmTagGetValue");
    }

    if (!missing.empty()) {
      g_module_close(module);
      init_error_kind_ = RpmQueryErrorKind::kUnsupportedAbi;
      unavailable_reason_ =
          "missing required symbols in " + library_name + ": ";
      for (size_t i = 0; i < missing.size(); ++i) {
        if (i > 0) unavailable_reason_.append(", ");
        unavailable_reason_.append(missing[i]);
      }
      return false;
    }

    load_symbol(module, "rpmfiNew", &syms.fi_new);
    load_symbol(module, "rpmfiFree", &syms.fi_free);
    load_symbol(module, "rpmfiInit", &syms.fi_init);
    load_symbol(module, "rpmfiNext", &syms.fi_next);
    load_symbol(module, "rpmfiFN", &syms.fi_fn);
    load_symbol(module, "rpmtsCloseDB", &syms.ts_close_db);
    load_symbol(module, "rpmtsSetRootDir", &syms.ts_set_root_dir);

    ResolvedRpmTags tags;
    tags.name = syms.tag_get_value("name");
    tags.epoch = syms.tag_get_value("epoch");
    tags.version = syms.tag_get_value("version");
    tags.release = syms.tag_get_value("release");
    tags.arch = syms.tag_get_value("arch");
    tags.summary = syms.tag_get_value("summary");
    tags.description = syms.tag_get_value("description");
    tags.license = syms.tag_get_value("license");
    tags.url = syms.tag_get_value("url");
    tags.vendor = syms.tag_get_value("vendor");
    tags.group = syms.tag_get_value("group");
    tags.basenames = syms.tag_get_value("basenames");

    if (tags.name == kRpmTagNotFound || tags.version == kRpmTagNotFound) {
      g_module_close(module);
      init_error_kind_ = RpmQueryErrorKind::kUnsupportedAbi;
      unavailable_reason_ =
          "librpm failed to resolve core RPM tag values in " + library_name;
      return false;
    }

    // Keep librpm resident once verified to avoid unloading global RPM state.
    g_module_make_resident(module);
    symbols_ = syms;
    tags_ = tags;
    available_ = true;
    abi_description_ = library_name;
    init_error_kind_ = RpmQueryErrorKind::kNone;
    unavailable_reason_.clear();
    return true;
  }

  void ensure_initialized() const {
    std::call_once(init_once_, [this]() {
      if (!g_module_supported()) {
        init_error_kind_ = RpmQueryErrorKind::kLibraryUnavailable;
        unavailable_reason_ = "GModule dynamic loading is not supported";
        return;
      }
      init_error_kind_ = RpmQueryErrorKind::kLibraryUnavailable;
      for (const auto& candidate : config_.library_candidates) {
        if (try_load_candidate(candidate)) {
          return;
        }
      }
      if (unavailable_reason_.empty()) {
        unavailable_reason_ =
            last_load_error_.empty()
                ? "no supported librpm shared library found"
                : last_load_error_;
      }
    });
  }

  LibRpmConfig config_;
  mutable std::once_flag init_once_;
  mutable bool available_ = false;
  mutable std::string abi_description_ = "unavailable";
  mutable RpmQueryErrorKind init_error_kind_ =
      RpmQueryErrorKind::kLibraryUnavailable;
  mutable std::string unavailable_reason_;
  mutable std::string last_load_error_;
  mutable LibRpmSymbols symbols_;
  mutable ResolvedRpmTags tags_;
};

}  // namespace

std::unique_ptr<RpmBackend> create_librpm_backend(LibRpmConfig config) {
  return std::unique_ptr<RpmBackend>(new LibRpmBackend(std::move(config)));
}

}  // namespace flutter_device_apps_linux
