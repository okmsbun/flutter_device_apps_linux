#include "include/flutter_device_apps_linux/flutter_device_apps_linux_plugin.h"

#include <gio/gio.h>

static void handle_method_call(FlMethodChannel*, FlMethodCall* call, gpointer) {
  if (g_strcmp0(fl_method_call_get_name(call), "listApps") != 0) {
    fl_method_call_respond_not_implemented(call, nullptr);
    return;
  }

  g_autoptr(FlValue) apps = fl_value_new_list();
  GList* app_infos = g_app_info_get_all();
  for (GList* item = app_infos; item != nullptr; item = item->next) {
    GAppInfo* info = G_APP_INFO(item->data);
    const char* desktop_id = g_app_info_get_id(info);
    if (desktop_id == nullptr || !g_app_info_should_show(info)) {
      continue;
    }

    FlValue* app = fl_value_new_map();
    fl_value_set_string_take(app, "desktopFileId",
                             fl_value_new_string(desktop_id));
    fl_value_set_string_take(app, "appName",
                             fl_value_new_string(g_app_info_get_display_name(info)));
    fl_value_append_take(apps, app);
  }
  g_list_free_full(app_infos, g_object_unref);
  fl_method_call_respond_success(call, apps, nullptr);
}

void flutter_device_apps_linux_plugin_register_with_registrar(
    FlPluginRegistrar* registrar) {
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar),
      "flutter_device_apps_linux/methods", FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, handle_method_call, nullptr,
                                            nullptr);
}
