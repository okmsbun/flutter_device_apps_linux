#include "libalpm_backend.h"

#include <gmodule.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flutter_device_apps_linux {
namespace {

// Public alpm_list_t layout stable across libalpm.so.13 through libalpm.so.16
// (see alpm_list.h). Traversal additionally uses the exported alpm_list_next()
// symbol to verify ABI compatibility.
struct AlpmListNode {
  void* data;
  AlpmListNode* prev;
  AlpmListNode* next;
};

// alpm_filelist_t / alpm_file_t layout (alpm.h, stable across so.13-16):
//   typedef struct { size_t count; alpm_file_t* files; } alpm_filelist_t;
//   typedef struct { char* name; off_t size; mode_t mode; } alpm_file_t;
// The element stride relies on a 64-bit off_t, which is verified at runtime.
struct AlpmFileEntry {
  char* name;
  off_t size;
  mode_t mode;
};
struct AlpmFileList {
  size_t count;
  AlpmFileEntry* files;
};

// alpm_pkgreason_t values in alpm.h across libalpm.so.13-16.
constexpr int kAlpmPkgReasonExplicit = 0;
constexpr int kAlpmPkgReasonDepend = 1;

struct LibAlpmSymbols {
  void* (*initialize)(const char*, const char*, int*) = nullptr;
  int (*release)(void*) = nullptr;
  int (*alpm_errno)(void*) = nullptr;
  const char* (*strerror)(int) = nullptr;
  void* (*get_localdb)(void*) = nullptr;
  AlpmListNode* (*db_get_pkgcache)(void*) = nullptr;
  AlpmListNode* (*list_next)(const AlpmListNode*) = nullptr;
  const char* (*pkg_get_name)(void*) = nullptr;
  const char* (*pkg_get_version)(void*) = nullptr;
  const char* (*pkg_get_arch)(void*) = nullptr;
  const char* (*pkg_get_desc)(void*) = nullptr;
  const char* (*pkg_get_url)(void*) = nullptr;
  const char* (*pkg_get_packager)(void*) = nullptr;
  AlpmListNode* (*pkg_get_licenses)(void*) = nullptr;
  AlpmListNode* (*pkg_get_groups)(void*) = nullptr;
  int (*pkg_get_reason)(void*) = nullptr;
  AlpmFileList* (*pkg_get_files)(void*) = nullptr;
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

OptionalString to_optional_string(const char* value) {
  if (value == nullptr || *value == '\0') return OptionalString::none();
  return OptionalString::of(value);
}

std::vector<std::string> read_string_list(const LibAlpmSymbols& syms,
                                          AlpmListNode* list) {
  std::vector<std::string> values;
  for (AlpmListNode* node = list; node != nullptr;
       node = syms.list_next(node)) {
    const char* str = static_cast<const char*>(node->data);
    if (str != nullptr && *str != '\0') {
      values.emplace_back(str);
    }
  }
  return values;
}

PackageInstallReason map_install_reason(int raw_reason) {
  if (raw_reason == kAlpmPkgReasonExplicit) {
    return PackageInstallReason::kExplicit;
  }
  if (raw_reason == kAlpmPkgReasonDepend) {
    return PackageInstallReason::kDependency;
  }
  return PackageInstallReason::kUnknown;
}

std::string join_path(const std::string& base, const char* child) {
  if (base.empty()) return child;
  if (base.back() == '/') return base + child;
  return base + "/" + child;
}

class LibAlpmBackend final : public PacmanBackend {
 public:
  explicit LibAlpmBackend(LibAlpmConfig config) : config_(std::move(config)) {}

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

  PacmanBackendQueryResult list_installed_packages() override {
    ensure_initialized();
    PacmanBackendQueryResult result;
    if (!available_) {
      result.ok = false;
      result.error_kind = init_error_kind_;
      result.error_message = unavailable_reason_;
      return result;
    }

    const PacmanResolvedPaths paths = resolve_pacman_db_paths(
        config_.pacman_conf_path, config_.root_dir, config_.db_path);

    // Pre-check database directory accessibility in read-only mode so missing
    // databases and permission denials produce accurate, actionable errors
    // without ever creating or touching database files.
    struct stat db_st;
    errno = 0;
    if (stat(paths.db_path.c_str(), &db_st) != 0) {
      const int err = errno;
      result.ok = false;
      result.error_kind = (err == EACCES || err == EPERM)
                              ? PacmanQueryErrorKind::kPermissionDenied
                              : PacmanQueryErrorKind::kDatabaseUnavailable;
      result.error_message = paths.db_path + ": " + std::strerror(err);
      return result;
    }
    if (!S_ISDIR(db_st.st_mode)) {
      result.ok = false;
      result.error_kind = PacmanQueryErrorKind::kDatabaseUnavailable;
      result.error_message = paths.db_path + " is not a directory";
      return result;
    }
    if (access(paths.db_path.c_str(), R_OK | X_OK) != 0) {
      const int err = errno;
      result.ok = false;
      result.error_kind = (err == EACCES || err == EPERM)
                              ? PacmanQueryErrorKind::kPermissionDenied
                              : PacmanQueryErrorKind::kDatabaseUnavailable;
      result.error_message = paths.db_path + ": " + std::strerror(err);
      return result;
    }

    const std::string local_dir = join_path(paths.db_path, "local");
    struct stat local_st;
    errno = 0;
    if (stat(local_dir.c_str(), &local_st) != 0) {
      const int err = errno;
      result.ok = false;
      result.error_kind = (err == EACCES || err == EPERM)
                              ? PacmanQueryErrorKind::kPermissionDenied
                              : PacmanQueryErrorKind::kDatabaseUnavailable;
      result.error_message = local_dir + ": " + std::strerror(err);
      return result;
    }
    if (!S_ISDIR(local_st.st_mode)) {
      result.ok = false;
      result.error_kind = PacmanQueryErrorKind::kDatabaseUnavailable;
      result.error_message = local_dir + " is not a directory";
      return result;
    }
    if (access(local_dir.c_str(), R_OK | X_OK) != 0) {
      const int err = errno;
      result.ok = false;
      result.error_kind = (err == EACCES || err == EPERM)
                              ? PacmanQueryErrorKind::kPermissionDenied
                              : PacmanQueryErrorKind::kDatabaseUnavailable;
      result.error_message = local_dir + ": " + std::strerror(err);
      return result;
    }

    struct ScopedAlpmHandle {
      const LibAlpmSymbols* syms = nullptr;
      void* handle = nullptr;

      ~ScopedAlpmHandle() {
        if (handle != nullptr && syms != nullptr) {
          syms->release(handle);
        }
      }
    };

    int init_err = 0;
    errno = 0;
    ScopedAlpmHandle guard{
        &symbols_,
        symbols_.initialize(paths.root_dir.c_str(), paths.db_path.c_str(),
                            &init_err)};
    if (guard.handle == nullptr) {
      const int sys_err = errno;
      const char* msg = symbols_.strerror(init_err);
      result.ok = false;
      result.error_kind = (sys_err == EACCES || sys_err == EPERM)
                              ? PacmanQueryErrorKind::kPermissionDenied
                              : PacmanQueryErrorKind::kDatabaseUnavailable;
      result.error_message =
          (msg != nullptr && *msg != '\0')
              ? std::string(msg)
              : std::string("alpm_initialize() failed");
      return result;
    }

    // Query ONLY the local installed package database; never register sync
    // repositories or start a transaction.
    void* localdb = symbols_.get_localdb(guard.handle);
    if (localdb == nullptr) {
      const int err_code = symbols_.alpm_errno(guard.handle);
      const char* msg = symbols_.strerror(err_code);
      result.ok = false;
      result.error_kind = PacmanQueryErrorKind::kDatabaseUnavailable;
      result.error_message =
          (msg != nullptr && *msg != '\0')
              ? std::string(msg)
              : std::string("alpm_get_localdb() returned null");
      return result;
    }

    // Returned alpm_list_t* and package pointers are owned by localdb and
    // released automatically by alpm_release() in ~ScopedAlpmHandle().
    AlpmListNode* pkgcache = symbols_.db_get_pkgcache(localdb);
    if (pkgcache == nullptr) {
      const int err_code = symbols_.alpm_errno(guard.handle);
      if (err_code != 0) {
        const char* msg = symbols_.strerror(err_code);
        result.ok = false;
        result.error_kind = PacmanQueryErrorKind::kDatabaseUnavailable;
        result.error_message =
            (msg != nullptr && *msg != '\0')
                ? std::string(msg)
                : std::string("alpm_db_get_pkgcache() failed");
        return result;
      }
      // Empty local database is valid and yields zero installed packages.
      result.ok = true;
      return result;
    }

    for (AlpmListNode* node = pkgcache; node != nullptr;
         node = symbols_.list_next(node)) {
      void* pkg = node->data;
      if (pkg == nullptr) continue;

      const char* raw_name = symbols_.pkg_get_name(pkg);
      if (raw_name == nullptr || *raw_name == '\0') continue;

      PacmanInstalledPackageRecord record;
      record.name = raw_name;
      record.version = to_optional_string(symbols_.pkg_get_version(pkg));
      record.architecture = to_optional_string(symbols_.pkg_get_arch(pkg));
      record.description = to_optional_string(symbols_.pkg_get_desc(pkg));
      record.homepage = to_optional_string(symbols_.pkg_get_url(pkg));
      record.packager = to_optional_string(symbols_.pkg_get_packager(pkg));
      record.licenses =
          read_string_list(symbols_, symbols_.pkg_get_licenses(pkg));
      record.groups = read_string_list(symbols_, symbols_.pkg_get_groups(pkg));
      record.install_reason = map_install_reason(symbols_.pkg_get_reason(pkg));

      result.packages.push_back(std::move(record));
    }

    result.ok = true;
    return result;
  }

  PacmanFileOwnershipResult find_file_owners(
      const std::vector<std::string>& absolute_paths) override {
    ensure_initialized();
    PacmanFileOwnershipResult result;
    if (!available_) {
      result.ok = false;
      result.error_kind = init_error_kind_;
      result.error_message = unavailable_reason_;
      return result;
    }
    if (symbols_.pkg_get_files == nullptr || sizeof(off_t) != 8) {
      result.ok = false;
      result.error_kind = PacmanQueryErrorKind::kUnsupportedAbi;
      result.error_message =
          "alpm_pkg_get_files is unavailable or off_t is not 64-bit";
      return result;
    }

    const PacmanResolvedPaths paths = resolve_pacman_db_paths(
        config_.pacman_conf_path, config_.root_dir, config_.db_path);
    const std::string local_dir = join_path(paths.db_path, "local");
    struct stat st;
    errno = 0;
    if (stat(local_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
        access(local_dir.c_str(), R_OK | X_OK) != 0) {
      const int err = errno;
      result.ok = false;
      result.error_kind = (err == EACCES || err == EPERM)
                              ? PacmanQueryErrorKind::kPermissionDenied
                              : PacmanQueryErrorKind::kDatabaseUnavailable;
      result.error_message =
          local_dir + (err != 0 ? std::string(": ") + std::strerror(err)
                                : std::string(" is not a readable directory"));
      return result;
    }

    // libalpm file names are relative to RootDir without a leading slash.
    std::string root = paths.root_dir;
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    const std::string root_prefix = root == "/" ? "/" : root + "/";
    std::unordered_map<std::string, std::string> relative_to_target;
    for (const auto& target : absolute_paths) {
      if (target.compare(0, root_prefix.size(), root_prefix) != 0) continue;
      const std::string rel = target.substr(root_prefix.size());
      if (!rel.empty()) relative_to_target[rel] = target;
    }
    if (relative_to_target.empty()) return result;

    int init_err = 0;
    void* handle = symbols_.initialize(paths.root_dir.c_str(),
                                       paths.db_path.c_str(), &init_err);
    if (handle == nullptr) {
      const char* msg = symbols_.strerror(init_err);
      result.ok = false;
      result.error_kind = PacmanQueryErrorKind::kDatabaseUnavailable;
      result.error_message = (msg != nullptr && *msg != '\0')
                                 ? std::string(msg)
                                 : std::string("alpm_initialize() failed");
      return result;
    }

    void* localdb = symbols_.get_localdb(handle);
    AlpmListNode* pkgcache =
        localdb != nullptr ? symbols_.db_get_pkgcache(localdb) : nullptr;
    for (AlpmListNode* node = pkgcache; node != nullptr;
         node = symbols_.list_next(node)) {
      void* pkg = node->data;
      if (pkg == nullptr) continue;
      const AlpmFileList* files = symbols_.pkg_get_files(pkg);
      if (files == nullptr || files->files == nullptr) continue;
      const char* raw_name = symbols_.pkg_get_name(pkg);
      if (raw_name == nullptr || *raw_name == '\0') continue;
      for (size_t i = 0; i < files->count; ++i) {
        const char* fname = files->files[i].name;
        if (fname == nullptr) continue;
        const auto hit = relative_to_target.find(fname);
        if (hit == relative_to_target.end()) continue;
        PacmanFileOwnerRecord owner;
        owner.path = hit->second;
        owner.package_name = raw_name;
        owner.architecture = to_optional_string(symbols_.pkg_get_arch(pkg));
        result.owners.push_back(std::move(owner));
      }
    }
    symbols_.release(handle);
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

    LibAlpmSymbols syms;
    std::vector<std::string> missing;
    if (!load_symbol(module, "alpm_initialize", &syms.initialize)) {
      missing.emplace_back("alpm_initialize");
    }
    if (!load_symbol(module, "alpm_release", &syms.release)) {
      missing.emplace_back("alpm_release");
    }
    if (!load_symbol(module, "alpm_errno", &syms.alpm_errno)) {
      missing.emplace_back("alpm_errno");
    }
    if (!load_symbol(module, "alpm_strerror", &syms.strerror)) {
      missing.emplace_back("alpm_strerror");
    }
    if (!load_symbol(module, "alpm_get_localdb", &syms.get_localdb)) {
      missing.emplace_back("alpm_get_localdb");
    }
    if (!load_symbol(module, "alpm_db_get_pkgcache", &syms.db_get_pkgcache)) {
      missing.emplace_back("alpm_db_get_pkgcache");
    }
    if (!load_symbol(module, "alpm_list_next", &syms.list_next)) {
      missing.emplace_back("alpm_list_next");
    }
    if (!load_symbol(module, "alpm_pkg_get_name", &syms.pkg_get_name)) {
      missing.emplace_back("alpm_pkg_get_name");
    }
    if (!load_symbol(module, "alpm_pkg_get_version", &syms.pkg_get_version)) {
      missing.emplace_back("alpm_pkg_get_version");
    }
    if (!load_symbol(module, "alpm_pkg_get_arch", &syms.pkg_get_arch)) {
      missing.emplace_back("alpm_pkg_get_arch");
    }
    if (!load_symbol(module, "alpm_pkg_get_desc", &syms.pkg_get_desc)) {
      missing.emplace_back("alpm_pkg_get_desc");
    }
    if (!load_symbol(module, "alpm_pkg_get_url", &syms.pkg_get_url)) {
      missing.emplace_back("alpm_pkg_get_url");
    }
    if (!load_symbol(module, "alpm_pkg_get_packager", &syms.pkg_get_packager)) {
      missing.emplace_back("alpm_pkg_get_packager");
    }
    if (!load_symbol(module, "alpm_pkg_get_licenses", &syms.pkg_get_licenses)) {
      missing.emplace_back("alpm_pkg_get_licenses");
    }
    if (!load_symbol(module, "alpm_pkg_get_groups", &syms.pkg_get_groups)) {
      missing.emplace_back("alpm_pkg_get_groups");
    }
    if (!load_symbol(module, "alpm_pkg_get_reason", &syms.pkg_get_reason)) {
      missing.emplace_back("alpm_pkg_get_reason");
    }
    // Optional: file ownership lookup degrades gracefully when absent.
    load_symbol(module, "alpm_pkg_get_files", &syms.pkg_get_files);

    if (!missing.empty()) {
      g_module_close(module);
      init_error_kind_ = PacmanQueryErrorKind::kUnsupportedAbi;
      unavailable_reason_ =
          "missing required symbols in " + library_name + ": ";
      for (size_t i = 0; i < missing.size(); ++i) {
        if (i > 0) unavailable_reason_.append(", ");
        unavailable_reason_.append(missing[i]);
      }
      return false;
    }

    // Keep libalpm resident once verified, matching libflatpak/libappstream/
    // librpm runtime loader behavior.
    g_module_make_resident(module);
    symbols_ = syms;
    available_ = true;
    abi_description_ = library_name;
    init_error_kind_ = PacmanQueryErrorKind::kNone;
    unavailable_reason_.clear();
    return true;
  }

  void ensure_initialized() const {
    std::call_once(init_once_, [this]() {
      if (!g_module_supported()) {
        init_error_kind_ = PacmanQueryErrorKind::kLibraryUnavailable;
        unavailable_reason_ = "GModule dynamic loading is not supported";
        return;
      }
      init_error_kind_ = PacmanQueryErrorKind::kLibraryUnavailable;
      for (const auto& candidate : config_.library_candidates) {
        if (try_load_candidate(candidate)) {
          return;
        }
      }
      if (unavailable_reason_.empty()) {
        unavailable_reason_ =
            last_load_error_.empty()
                ? "no supported libalpm shared library found"
                : last_load_error_;
      }
    });
  }

  LibAlpmConfig config_;
  mutable std::once_flag init_once_;
  mutable bool available_ = false;
  mutable std::string abi_description_ = "unavailable";
  mutable PacmanQueryErrorKind init_error_kind_ =
      PacmanQueryErrorKind::kLibraryUnavailable;
  mutable std::string unavailable_reason_;
  mutable std::string last_load_error_;
  mutable LibAlpmSymbols symbols_;
};

}  // namespace

std::unique_ptr<PacmanBackend> create_libalpm_backend(LibAlpmConfig config) {
  return std::unique_ptr<PacmanBackend>(new LibAlpmBackend(std::move(config)));
}

}  // namespace flutter_device_apps_linux
