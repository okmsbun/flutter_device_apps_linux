#include "libflatpak_backend.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <gmodule.h>

#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace flutter_device_apps_linux {
namespace {

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

using ScopedGObject = std::unique_ptr<void, GObjectDeleter>;
using ScopedGPtrArray = std::unique_ptr<GPtrArray, GPtrArrayDeleter>;

struct LibFlatpakSymbols {
  // Core installation discovery symbols (libflatpak >= 0.8.0 for
  // flatpak_get_system_installations; fallback to new_system on older builds).
  GPtrArray* (*get_system_installations)(GCancellable*, GError**) = nullptr;
  gpointer (*installation_new_system)(GCancellable*, GError**) = nullptr;
  gpointer (*installation_new_user)(GCancellable*, GError**) = nullptr;
  GPtrArray* (*installation_list_installed_refs_by_kind)(
      gpointer, int, GCancellable*, GError**) = nullptr;
  const char* (*installation_get_id)(gpointer) = nullptr;
  gboolean (*installation_get_is_user)(gpointer) = nullptr;

  // Core ref inspection symbols.
  const char* (*ref_get_name)(gpointer) = nullptr;
  const char* (*ref_get_arch)(gpointer) = nullptr;
  const char* (*ref_get_branch)(gpointer) = nullptr;
  int (*ref_get_kind)(gpointer) = nullptr;

  // Optional symbols (libflatpak >= 1.1.1 / >= 1.1.2).
  void (*installation_set_no_interaction)(gpointer, gboolean) = nullptr;
  const char* (*installed_ref_get_appdata_name)(gpointer) = nullptr;
  const char* (*installed_ref_get_appdata_summary)(gpointer) = nullptr;
  const char* (*installed_ref_get_appdata_version)(gpointer) = nullptr;
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

class LibFlatpakBackend final : public FlatpakBackend {
 public:
  explicit LibFlatpakBackend(const char* library_name) {
    if (library_name == nullptr || *library_name == '\0' ||
        !g_module_supported()) {
      return;
    }

    GModule* module = g_module_open(
        library_name, static_cast<GModuleFlags>(G_MODULE_BIND_LOCAL |
                                                G_MODULE_BIND_LAZY));
    if (module == nullptr) {
      return;
    }

    const bool has_system_list = load_symbol(
        module, "flatpak_get_system_installations",
        &symbols_.get_system_installations);
    const bool has_system_single = load_symbol(
        module, "flatpak_installation_new_system",
        &symbols_.installation_new_system);

    const bool core_ok =
        (has_system_list || has_system_single) &&
        load_symbol(module, "flatpak_installation_new_user",
                    &symbols_.installation_new_user) &&
        load_symbol(module, "flatpak_installation_list_installed_refs_by_kind",
                    &symbols_.installation_list_installed_refs_by_kind) &&
        load_symbol(module, "flatpak_installation_get_id",
                    &symbols_.installation_get_id) &&
        load_symbol(module, "flatpak_installation_get_is_user",
                    &symbols_.installation_get_is_user) &&
        load_symbol(module, "flatpak_ref_get_name", &symbols_.ref_get_name) &&
        load_symbol(module, "flatpak_ref_get_arch", &symbols_.ref_get_arch) &&
        load_symbol(module, "flatpak_ref_get_branch",
                    &symbols_.ref_get_branch) &&
        load_symbol(module, "flatpak_ref_get_kind", &symbols_.ref_get_kind);

    if (!core_ok) {
      g_module_close(module);
      symbols_ = LibFlatpakSymbols{};
      return;
    }

    // Optional symbols degrade gracefully on older libflatpak versions.
    load_symbol(module, "flatpak_installation_set_no_interaction",
                &symbols_.installation_set_no_interaction);
    load_symbol(module, "flatpak_installed_ref_get_appdata_name",
                &symbols_.installed_ref_get_appdata_name);
    load_symbol(module, "flatpak_installed_ref_get_appdata_summary",
                &symbols_.installed_ref_get_appdata_summary);
    load_symbol(module, "flatpak_installed_ref_get_appdata_version",
                &symbols_.installed_ref_get_appdata_version);

    // libflatpak registers static GTypes in GLib's global type registry; keep
    // the loaded module resident once core symbols are verified.
    g_module_make_resident(module);
    available_ = true;
  }

  bool is_available() const override { return available_; }

  FlatpakBackendQueryResult list_installed_apps() override {
    FlatpakBackendQueryResult result;
    if (!available_) {
      result.ok = false;
      result.error_message = "libflatpak is not available";
      return result;
    }

    // 1. Discover user installation.
    {
      g_autoptr(GError) error = nullptr;
      ScopedGObject user_installation(
          symbols_.installation_new_user(nullptr, &error));
      if (user_installation == nullptr) {
        FlatpakInstallationQueryResult failed;
        failed.installation_id = "user";
        failed.is_user = true;
        failed.ok = false;
        failed.error_message =
            error != nullptr && error->message != nullptr
                ? error->message
                : "Unable to open user Flatpak installation";
        result.installations.push_back(std::move(failed));
      } else {
        result.installations.push_back(
            query_installation(user_installation.get(), true));
      }
    }

    // 2. Discover all configured system installations (including named ones).
    if (symbols_.get_system_installations != nullptr) {
      g_autoptr(GError) error = nullptr;
      ScopedGPtrArray system_installations(
          symbols_.get_system_installations(nullptr, &error));
      if (system_installations == nullptr) {
        FlatpakInstallationQueryResult failed;
        failed.installation_id = "system";
        failed.is_user = false;
        failed.ok = false;
        failed.error_message =
            error != nullptr && error->message != nullptr
                ? error->message
                : "Unable to enumerate system Flatpak installations";
        result.installations.push_back(std::move(failed));
      } else {
        for (guint i = 0; i < system_installations->len; ++i) {
          gpointer installation = g_ptr_array_index(system_installations, i);
          if (installation == nullptr) continue;
          const bool is_user =
              symbols_.installation_get_is_user(installation) != FALSE;
          result.installations.push_back(
              query_installation(installation, is_user));
        }
      }
    } else if (symbols_.installation_new_system != nullptr) {
      g_autoptr(GError) error = nullptr;
      ScopedGObject system_installation(
          symbols_.installation_new_system(nullptr, &error));
      if (system_installation == nullptr) {
        FlatpakInstallationQueryResult failed;
        failed.installation_id = "default";
        failed.is_user = false;
        failed.ok = false;
        failed.error_message =
            error != nullptr && error->message != nullptr
                ? error->message
                : "Unable to open system Flatpak installation";
        result.installations.push_back(std::move(failed));
      } else {
        result.installations.push_back(
            query_installation(system_installation.get(), false));
      }
    }

    return result;
  }

 private:
  FlatpakInstallationQueryResult query_installation(gpointer installation,
                                                    bool default_is_user) const {
    FlatpakInstallationQueryResult out;
    out.is_user = symbols_.installation_get_is_user(installation) != FALSE ||
                  default_is_user;
    const char* raw_id = symbols_.installation_get_id(installation);
    out.installation_id =
        raw_id != nullptr && *raw_id != '\0'
            ? raw_id
            : (out.is_user ? "user" : "default");

    if (symbols_.installation_set_no_interaction != nullptr) {
      symbols_.installation_set_no_interaction(installation, TRUE);
    }

    g_autoptr(GError) error = nullptr;
    ScopedGPtrArray refs(symbols_.installation_list_installed_refs_by_kind(
        installation, kFlatpakRefKindApp, nullptr, &error));
    if (refs == nullptr) {
      out.ok = false;
      out.error_message =
          error != nullptr && error->message != nullptr
              ? error->message
              : "Unable to list installed Flatpak applications";
      return out;
    }

    out.apps.reserve(refs->len);
    for (guint i = 0; i < refs->len; ++i) {
      gpointer ref = g_ptr_array_index(refs, i);
      if (ref == nullptr) continue;

      const int kind = symbols_.ref_get_kind(ref);
      if (kind != kFlatpakRefKindApp) continue;

      const char* app_id = symbols_.ref_get_name(ref);
      if (app_id == nullptr || *app_id == '\0') continue;

      FlatpakInstalledAppRecord record;
      record.app_id = app_id;
      record.ref_kind = kind;

      const char* arch = symbols_.ref_get_arch(ref);
      if (arch != nullptr) {
        record.arch = arch;
      }
      const char* branch = symbols_.ref_get_branch(ref);
      if (branch != nullptr) {
        record.branch = branch;
      }

      if (symbols_.installed_ref_get_appdata_name != nullptr) {
        const char* name = symbols_.installed_ref_get_appdata_name(ref);
        if (name != nullptr) {
          record.name = OptionalString::of(name);
        }
      }
      if (symbols_.installed_ref_get_appdata_version != nullptr) {
        const char* version = symbols_.installed_ref_get_appdata_version(ref);
        if (version != nullptr) {
          record.version = OptionalString::of(version);
        }
      }
      if (symbols_.installed_ref_get_appdata_summary != nullptr) {
        const char* summary = symbols_.installed_ref_get_appdata_summary(ref);
        if (summary != nullptr) {
          record.summary = OptionalString::of(summary);
        }
      }

      out.apps.push_back(std::move(record));
    }

    return out;
  }

  bool available_ = false;
  LibFlatpakSymbols symbols_;
};

}  // namespace

std::unique_ptr<FlatpakBackend> create_libflatpak_backend(
    const char* library_name) {
  return std::unique_ptr<FlatpakBackend>(new LibFlatpakBackend(library_name));
}

}  // namespace flutter_device_apps_linux
