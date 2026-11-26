#include "gio_discovery_provider.h"

#include <gio/gdesktopappinfo.h>

#include <memory>
#include <utility>

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

DiscoveryResult GioDiscoveryProvider::discover() {
  DiscoveryResult result;
  ScopedAppInfoList infos(g_app_info_get_all());

  for (GList* item = infos.get(); item != nullptr; item = item->next) {
    if (item->data == nullptr) continue;
    GAppInfo* info = G_APP_INFO(item->data);
    if (!g_app_info_should_show(info)) continue;

    const char* desktop_file_id = g_app_info_get_id(info);
    if (desktop_file_id == nullptr) continue;

    DiscoveredApplication app;
    app.desktop_file_id = desktop_file_id;

    const char* display_name = g_app_info_get_display_name(info);
    if (display_name != nullptr) {
      app.app_name = display_name;
      app.has_app_name = true;
    }

    auto* desktop =
        G_IS_DESKTOP_APP_INFO(info) ? G_DESKTOP_APP_INFO(info) : nullptr;
    app.supports_files = g_app_info_supports_files(info) != FALSE;
    app.supports_uris = g_app_info_supports_uris(info) != FALSE;
    app.requires_terminal =
        desktop != nullptr &&
        g_desktop_app_info_get_boolean(desktop, "Terminal") != FALSE;

    result.applications.push_back(std::move(app));
  }

  return result;
}

}  // namespace flutter_device_apps_linux
