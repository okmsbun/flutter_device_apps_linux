#include "gio_discovery_provider.h"

#include <gio/gdesktopappinfo.h>

#include <memory>
#include <utility>

#include "application_identity.h"

namespace flutter_device_apps_linux {
namespace {

struct AppInfoListDeleter {
  void operator()(GList* list) const {
    if (list != nullptr) {
      g_list_free_full(list, g_object_unref);
    }
  }
};

using ScopedAppInfoList = std::unique_ptr<GList, AppInfoListDeleter>;

}  // namespace

const char* GioDiscoveryProvider::id() const {
  return "gio";
}

bool GioDiscoveryProvider::is_available() const {
  return true;
}

// Thread affinity note:
// - Uses only GLib/GIO desktop entry APIs (no GTK/GDK or Flutter APIs).
// - g_app_info_get_all() guards directory cache initialization internally via
//   GLib locks, and g_app_info_should_show() is safe on a worker thread as long
//   as process environment variables (such as XDG_CURRENT_DESKTOP) are not
//   mutated concurrently.
// - All returned DiscoveredApplication values own their data; no GAppInfo* or
//   borrowed GLib strings cross the worker boundary.
DiscoveryResult GioDiscoveryProvider::discover() {
  DiscoveryResult result;
  ScopedAppInfoList infos(g_app_info_get_all());

  for (GList* item = infos.get(); item != nullptr; item = item->next) {
    if (item->data == nullptr) continue;
    GAppInfo* info = G_APP_INFO(item->data);

    const char* desktop_file_id = g_app_info_get_id(info);
    if (desktop_file_id == nullptr) continue;

    DiscoveredApplication app;
    app.provenance.provider_id = id();
    app.provenance.native_id = desktop_file_id;
    app.provenance.installation_source = InstallationSource::kUnknown;
    app.provenance.installation_scope = InstallationScope::kUnknown;
    app.application_id = build_application_id(
        app.provenance.provider_id, app.provenance.native_id,
        app.provenance.installation_scope, app.installation_id);
    app.desktop_file_id = OptionalString::of(desktop_file_id);
    app.desktop_menu_visibility = g_app_info_should_show(info) != FALSE
                                      ? DesktopMenuVisibility::kVisible
                                      : DesktopMenuVisibility::kNotVisible;

    const char* display_name = g_app_info_get_display_name(info);
    if (display_name != nullptr) {
      app.metadata.name = OptionalString::of(display_name);
    }

    auto* desktop =
        G_IS_DESKTOP_APP_INFO(info) ? G_DESKTOP_APP_INFO(info) : nullptr;
    app.capabilities.supports_files =
        capability_from_bool(g_app_info_supports_files(info) != FALSE);
    app.capabilities.supports_uris =
        capability_from_bool(g_app_info_supports_uris(info) != FALSE);
    app.capabilities.requires_terminal = capability_from_bool(
        desktop != nullptr &&
        g_desktop_app_info_get_boolean(desktop, "Terminal") != FALSE);

    result.applications.push_back(std::move(app));
  }

  return result;
}

}  // namespace flutter_device_apps_linux
