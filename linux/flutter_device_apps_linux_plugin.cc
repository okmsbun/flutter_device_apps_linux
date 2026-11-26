#include "include/flutter_device_apps_linux/flutter_device_apps_linux_plugin.h"

#include "discovery/application_discovery_service.h"
#include "discovery/appstream_metadata_provider.h"
#include "discovery/async_discovery_runner.h"
#include "discovery/dpkg_discovery_provider.h"
#include "discovery/flatpak_discovery_provider.h"
#include "discovery/gio_discovery_provider.h"
#include "discovery/legacy_list_apps_adapter.h"
#include "discovery/libalpm_backend.h"
#include "discovery/libappstream_backend.h"
#include "discovery/libflatpak_backend.h"
#include "discovery/librpm_backend.h"
#include "discovery/pacman_discovery_provider.h"
#include "discovery/rpm_discovery_provider.h"
#include "discovery/snap_discovery_provider.h"
#include "discovery/snapd_socket_backend.h"
#include "elf_dependencies.h"

#include <gio/gdesktopappinfo.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

static void set_string(FlValue* map, const char* key, const char* value) {
  if (value != nullptr) {
    fl_value_set_string_take(map, key, fl_value_new_string(value));
  }
}

static FlValue* string_list(const char* const* values) {
  FlValue* list = fl_value_new_list();
  for (; values != nullptr && *values != nullptr; ++values) {
    if (**values != '\0') {
      fl_value_append_take(list, fl_value_new_string(*values));
    }
  }
  return list;
}

static gchar* utf8_path(const char* path) {
  return path == nullptr ? nullptr
                         : g_filename_to_utf8(path, -1, nullptr, nullptr, nullptr);
}

static FlValue* app_value(GAppInfo* info, bool include_details = false) {
  if (info == nullptr || g_app_info_get_id(info) == nullptr) return nullptr;
  FlValue* app = fl_value_new_map();
  set_string(app, "desktopFileId", g_app_info_get_id(info));
  set_string(app, "appName", g_app_info_get_display_name(info));
  auto* desktop =
      G_IS_DESKTOP_APP_INFO(info) ? G_DESKTOP_APP_INFO(info) : nullptr;
  fl_value_set_string_take(app, "supportsFiles",
                           fl_value_new_bool(g_app_info_supports_files(info)));
  fl_value_set_string_take(app, "supportsUris",
                           fl_value_new_bool(g_app_info_supports_uris(info)));
  fl_value_set_string_take(app, "requiresTerminal", fl_value_new_bool(
      desktop != nullptr && g_desktop_app_info_get_boolean(desktop, "Terminal")));
  if (!include_details || desktop == nullptr) return app;

  set_string(app, "description", g_app_info_get_description(info));
  g_autofree char* path = utf8_path(g_desktop_app_info_get_filename(desktop));
  set_string(app, "desktopFilePath", path);
  const char* category_string = g_desktop_app_info_get_categories(desktop);
  g_auto(GStrv) categories =
      category_string == nullptr ? nullptr : g_strsplit(category_string, ";", -1);
  fl_value_set_string_take(app, "categories", string_list(categories));
  fl_value_set_string_take(app, "mimeTypes",
                           string_list(g_app_info_get_supported_types(info)));
  return app;
}

static FlValue* app_list(GList* infos, bool only_visible) {
  FlValue* apps = fl_value_new_list();
  for (GList* item = infos; item != nullptr; item = item->next) {
    GAppInfo* info = G_APP_INFO(item->data);
    if (only_visible && !g_app_info_should_show(info)) continue;
    FlValue* app = app_value(info, !only_visible);
    if (app != nullptr) fl_value_append_take(apps, app);
  }
  g_list_free_full(infos, g_object_unref);
  return apps;
}

static FlValue* legacy_app_value(
    const flutter_device_apps_linux::LegacyDesktopAppRecord& app) {
  FlValue* value = fl_value_new_map();
  set_string(value, "desktopFileId", app.desktop_file_id.c_str());
  if (app.app_name.has_value()) {
    set_string(value, "appName", app.app_name.value().c_str());
  }
  fl_value_set_string_take(value, "supportsFiles",
                           fl_value_new_bool(app.supports_files));
  fl_value_set_string_take(value, "supportsUris",
                           fl_value_new_bool(app.supports_uris));
  fl_value_set_string_take(value, "requiresTerminal",
                           fl_value_new_bool(app.requires_terminal));
  return value;
}

static FlValue* legacy_list_apps_value(
    const std::vector<flutter_device_apps_linux::DiscoveredApplication>& apps) {
  const auto legacy_records =
      flutter_device_apps_linux::adapt_for_legacy_list_apps(apps);
  FlValue* list = fl_value_new_list();
  for (const auto& record : legacy_records) {
    fl_value_append_take(list, legacy_app_value(record));
  }
  return list;
}

static FlValue* search_apps(const char* query) {
  FlValue* result = fl_value_new_list();
  if (*query == '\0') return result;
  g_autofree gchar*** groups = g_desktop_app_info_search(query);
  g_autoptr(GHashTable) seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
  for (auto group = groups; group != nullptr && *group != nullptr; ++group) {
    for (auto id = *group; *id != nullptr; ++id) {
      if (!g_hash_table_add(seen, g_strdup(*id))) continue;
      g_autoptr(GDesktopAppInfo) desktop = g_desktop_app_info_new(*id);
      if (desktop == nullptr) continue;
      GAppInfo* info = G_APP_INFO(desktop);
      if (g_app_info_should_show(info)) {
        fl_value_append_take(result, app_value(info));
      }
    }
    g_strfreev(*group);
  }
  return result;
}

static FlValue* app_icon(GAppInfo* info) {
  GIcon* icon = g_app_info_get_icon(info);
  GtkIconTheme* theme = gdk_screen_get_default() == nullptr
                            ? nullptr : gtk_icon_theme_get_default();
  if (icon == nullptr || theme == nullptr) return nullptr;
  g_autoptr(GtkIconInfo) icon_info = gtk_icon_theme_lookup_by_gicon(
      theme, icon, 64, GTK_ICON_LOOKUP_FORCE_SIZE);
  if (icon_info == nullptr) return nullptr;
  g_autoptr(GdkPixbuf) pixbuf = gtk_icon_info_load_icon(icon_info, nullptr);
  if (pixbuf == nullptr) return nullptr;

  g_autofree char* png = nullptr;
  gsize length = 0;
  if (!gdk_pixbuf_save_to_buffer(pixbuf, &png, &length, "png", nullptr, nullptr)) {
    return nullptr;
  }
  return fl_value_new_uint8_list(reinterpret_cast<const uint8_t*>(png), length);
}

static void open_app(FlMethodCall* call, GAppInfo* info) {
  GdkDisplay* display = gdk_display_get_default();
  g_autoptr(GdkAppLaunchContext) context = display == nullptr
      ? nullptr : gdk_display_get_app_launch_context(display);
  if (context != nullptr) {
    gdk_app_launch_context_set_timestamp(context, gtk_get_current_event_time());
  }
  g_autoptr(GError) error = nullptr;
  if (!g_app_info_launch(info, nullptr, G_APP_LAUNCH_CONTEXT(context), &error)) {
    fl_method_call_respond_error(
        call, "ERR_OPEN", error == nullptr ? "Unable to launch application"
                                          : error->message, nullptr, nullptr);
    return;
  }
  g_autoptr(FlValue) success = fl_value_new_bool(true);
  fl_method_call_respond_success(call, success, nullptr);
}

struct InstallSource {
  const char* source;
  std::string package_id;
  std::string version_name;
};

static InstallSource install_source(GDesktopAppInfo* desktop) {
  g_autofree char* flatpak = g_desktop_app_info_get_string(desktop, "X-Flatpak");
  if (flatpak != nullptr && *flatpak != '\0') return {"flatpak", flatpak, {}};
  g_autofree char* snap = g_desktop_app_info_get_string(desktop, "X-SnapInstanceName");
  if (snap != nullptr && *snap != '\0') return {"snap", snap, {}};

  g_autofree char* version = g_desktop_app_info_get_string(desktop, "X-AppImage-Version");
  g_autofree char* name = g_desktop_app_info_get_string(desktop, "X-AppImage-Name");
  g_autofree char* arch = g_desktop_app_info_get_string(desktop, "X-AppImage-Arch");
  if ((version != nullptr && *version != '\0') ||
      (name != nullptr && *name != '\0') || (arch != nullptr && *arch != '\0')) {
    return {"appImage", {}, version == nullptr ? "" : version};
  }
  return {"unknown", {}, {}};
}

static FlValue* install_source_value(GDesktopAppInfo* desktop) {
  const auto info = install_source(desktop);
  FlValue* result = fl_value_new_map();
  set_string(result, "source", info.source);
  if (!info.package_id.empty()) set_string(result, "packageId", info.package_id.c_str());
  if (!info.version_name.empty()) set_string(result, "versionName", info.version_name.c_str());
  return result;
}

static FlValue* appstream_locations(GDesktopAppInfo* desktop) {
  FlValue* result = fl_value_new_map();
  g_autofree char* path = utf8_path(g_desktop_app_info_get_filename(desktop));
  set_string(result, "desktopFilePath", path);

  FlValue* directories = fl_value_new_list();
  g_autofree char* user_dir = utf8_path(g_get_user_data_dir());
  if (user_dir != nullptr) {
    fl_value_append_take(directories, fl_value_new_string(user_dir));
  }
  for (auto dir = g_get_system_data_dirs(); *dir != nullptr; ++dir) {
    g_autofree char* system_dir = utf8_path(*dir);
    if (system_dir != nullptr) {
      fl_value_append_take(directories, fl_value_new_string(system_dir));
    }
  }
  fl_value_set_string_take(result, "dataDirectories", directories);
  fl_value_set_string_take(result, "languages", string_list(g_get_language_names()));
  return result;
}

static const char* string_argument(FlMethodCall* call) {
  FlValue* args = fl_method_call_get_args(call);
  return args != nullptr && fl_value_get_type(args) == FL_VALUE_TYPE_STRING
             ? fl_value_get_string(args)
             : nullptr;
}

static FlValue* content_handlers(const char* type, bool default_only) {
  if (!default_only) {
    return app_list(type == nullptr ? nullptr : g_app_info_get_all_for_type(type), false);
  }
  g_autoptr(GAppInfo) handler = type == nullptr ? nullptr
      : g_app_info_get_default_for_type(type, FALSE);
  return app_value(handler, true);
}

static FlValue* file_handlers(const char* path, bool default_only, GError** error) {
  g_autofree char* filename = g_filename_from_utf8(path, -1, nullptr, nullptr, error);
  if (filename == nullptr) return nullptr;
  g_autoptr(GFile) file = g_file_new_for_path(filename);
  g_autoptr(GFileInfo) file_info = g_file_query_info(
      file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE,
      nullptr, error);
  if (g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
    g_clear_error(error);
  }
  if (*error != nullptr) return nullptr;
  const char* type = file_info == nullptr ? nullptr : g_file_info_get_content_type(file_info);
  return content_handlers(type, default_only);
}

static gchar* elf_path(GDesktopAppInfo* desktop) {
  if (!g_str_equal(install_source(desktop).source, "unknown")) return nullptr;
  const char* command = g_app_info_get_commandline(G_APP_INFO(desktop));
  g_auto(GStrv) arguments = nullptr;
  if (command == nullptr ||
      !g_shell_parse_argv(command, nullptr, &arguments, nullptr) ||
      *arguments[0] == '\0') {
    return nullptr;
  }
  const char* executable = arguments[0];
  g_autofree char* name = g_path_get_basename(executable);
  for (const auto* wrapper : {"env", "sh", "bash", "dash", "zsh", "flatpak", "snap", "bwrap"}) {
    if (g_str_equal(name, wrapper)) return nullptr;
  }
  if (g_path_is_absolute(executable)) return g_strdup(executable);
  if (g_strstr_len(executable, -1, "/") != nullptr) return nullptr;
  return g_find_program_in_path(executable);
}

enum class QueryKind {
  default_handler, file_handlers, default_mime_handler, mime_handlers, search, elf,
};

struct Query {
  FlMethodCall* call;
  QueryKind kind;
  std::string target;

  ~Query() { g_object_unref(call); }
};

static void finish_query(GObject*, GAsyncResult* result, gpointer) {
  GTask* task = G_TASK(result);
  const auto* query = static_cast<Query*>(g_task_get_task_data(task));
  g_autoptr(GError) error = nullptr;
  g_autoptr(FlValue) response =
      static_cast<FlValue*>(g_task_propagate_pointer(task, &error));
  if (error != nullptr) {
    fl_method_call_respond_error(query->call, "ERR_FILE_HANDLERS",
                                 error->message, nullptr, nullptr);
  } else {
    fl_method_call_respond_success(query->call, response, nullptr);
  }
}

static void run_query(GTask* task, gpointer, gpointer data, GCancellable*) {
  const auto* query = static_cast<Query*>(data);
  g_autoptr(GError) error = nullptr;
  FlValue* response = nullptr;
  switch (query->kind) {
    case QueryKind::elf: {
      g_autoptr(GDesktopAppInfo) desktop = g_desktop_app_info_new(query->target.c_str());
      if (desktop != nullptr) {
        response = fl_value_new_list();
        g_autofree char* path = elf_path(desktop);
        if (path != nullptr) {
          for (const auto& name : read_elf_dependencies(path)) {
            fl_value_append_take(response, fl_value_new_string(name.c_str()));
          }
        }
      }
      break;
    }
    case QueryKind::search:
      response = search_apps(query->target.c_str());
      break;
    case QueryKind::default_mime_handler:
    case QueryKind::mime_handlers: {
      g_autofree char* mime = g_ascii_strdown(query->target.c_str(), -1);
      g_autofree char* type = g_content_type_from_mime_type(mime);
      response = content_handlers(type, query->kind == QueryKind::default_mime_handler);
      break;
    }
    case QueryKind::default_handler:
    case QueryKind::file_handlers:
      response = file_handlers(query->target.c_str(),
                               query->kind == QueryKind::default_handler, &error);
      break;
  }
  if (error != nullptr) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }
  g_task_return_pointer(task, response, [](gpointer value) {
    if (value != nullptr) fl_value_unref(static_cast<FlValue*>(value));
  });
}

static void schedule_query(FlMethodCall* call, QueryKind kind, const char* target) {
  g_autoptr(GTask) task = g_task_new(nullptr, nullptr, finish_query, nullptr);
  g_task_set_task_data(
      task, new Query{static_cast<FlMethodCall*>(g_object_ref(call)), kind, target},
      [](gpointer data) { delete static_cast<Query*>(data); });
  g_task_run_in_thread(task, run_query);
}

struct ScopedMethodCall {
  FlMethodCall* call = nullptr;

  explicit ScopedMethodCall(FlMethodCall* retained) : call(retained) {}
  ~ScopedMethodCall() {
    if (call != nullptr) {
      g_object_unref(call);
    }
  }

  ScopedMethodCall(const ScopedMethodCall&) = delete;
  ScopedMethodCall& operator=(const ScopedMethodCall&) = delete;
};

// Owned and accessed exclusively on the Flutter Linux platform thread.
// Keeps FlMethodCall* ownership strictly off worker threads so pending calls
// are released immediately on plugin teardown even if the platform main loop
// stops before an in-flight worker completes.
struct PendingMethodCalls {
  bool torn_down = false;
  uint64_t next_id = 0;
  std::unordered_map<uint64_t, FlMethodCall*> calls;

  ~PendingMethodCalls() { teardown(); }

  uint64_t add(FlMethodCall* call) {
    const uint64_t id = ++next_id;
    calls.emplace(id, static_cast<FlMethodCall*>(g_object_ref(call)));
    return id;
  }

  FlMethodCall* take(uint64_t id) {
    if (torn_down) return nullptr;
    const auto it = calls.find(id);
    if (it == calls.end()) return nullptr;
    FlMethodCall* call = it->second;
    calls.erase(it);
    return call;
  }

  void teardown() {
    torn_down = true;
    for (auto& entry : calls) {
      if (entry.second != nullptr) {
        g_object_unref(entry.second);
      }
    }
    calls.clear();
  }
};

struct DiscoveryPluginContext {
  std::shared_ptr<flutter_device_apps_linux::ApplicationDiscoveryService>
      service;
  std::unique_ptr<flutter_device_apps_linux::AsyncDiscoveryRunner> runner;
  std::shared_ptr<PendingMethodCalls> pending_calls =
      std::make_shared<PendingMethodCalls>();

  ~DiscoveryPluginContext() {
    if (pending_calls != nullptr) {
      pending_calls->teardown();
    }
    if (runner != nullptr) {
      runner->shutdown();
    }
  }
};

static void handle_method_call(FlMethodChannel*, FlMethodCall* call,
                               gpointer user_data) {
  const char* method = fl_method_call_get_name(call);
  if (g_str_equal(method, "listApps")) {
    auto* context = static_cast<DiscoveryPluginContext*>(user_data);
    if (context == nullptr || context->runner == nullptr ||
        context->pending_calls == nullptr) {
      g_autoptr(FlValue) empty = fl_value_new_list();
      fl_method_call_respond_success(call, empty, nullptr);
      return;
    }

    const uint64_t call_id = context->pending_calls->add(call);
    std::weak_ptr<PendingMethodCalls> weak_calls(context->pending_calls);

    context->runner->discover_async(
        [call_id, weak_calls](
            flutter_device_apps_linux::DiscoveryExecutionOutcome outcome) {
          auto pending_calls = weak_calls.lock();
          if (pending_calls == nullptr) return;

          ScopedMethodCall method_call(pending_calls->take(call_id));
          if (method_call.call == nullptr) return;
          if (outcome.status ==
              flutter_device_apps_linux::DiscoveryExecutionStatus::kCancelled) {
            return;
          }
          if (outcome.status ==
              flutter_device_apps_linux::DiscoveryExecutionStatus::
                  kExecutionError) {
            fl_method_call_respond_error(method_call.call, "ERR_DISCOVERY",
                                         "Application discovery failed",
                                         nullptr, nullptr);
            return;
          }
          g_autoptr(FlValue) apps = legacy_list_apps_value(outcome.applications);
          fl_method_call_respond_success(method_call.call, apps, nullptr);
        },
        flutter_device_apps_linux::DiscoveryRequestOptions::only_providers(
            {"gio"}));
    return;
  }

  const char* target = string_argument(call);
  if (g_str_equal(method, "searchApps")) {
    if (target == nullptr) {
      fl_method_call_respond_error(call, "ARG", "Expected a search string",
                                   nullptr, nullptr);
      return;
    }
    g_autofree char* query = g_strdup(target);
    schedule_query(call, QueryKind::search, g_strstrip(query));
    return;
  }

  const bool default_mime = g_str_equal(method, "getDefaultAppForMimeType");
  if (default_mime || g_str_equal(method, "listAppsForMimeType")) {
    if (target == nullptr || !g_regex_match_simple(
            "\\A[A-Za-z0-9!#$&^_.+-]+/[A-Za-z0-9!#$&^_.+-]+\\z", target,
            static_cast<GRegexCompileFlags>(0), static_cast<GRegexMatchFlags>(0))) {
      fl_method_call_respond_error(call, "ARG", "Expected a MIME type",
                                   nullptr, nullptr);
      return;
    }
    schedule_query(call, default_mime ? QueryKind::default_mime_handler
                                       : QueryKind::mime_handlers, target);
    return;
  }

  const bool default_handler = g_str_equal(method, "getDefaultAppForFile");
  if (default_handler || g_str_equal(method, "listAppsForFile")) {
    if (target == nullptr || !g_path_is_absolute(target)) {
      fl_method_call_respond_error(call, "ARG", "Expected an absolute file path",
                                   nullptr, nullptr);
      return;
    }
    schedule_query(call, default_handler ? QueryKind::default_handler
                                         : QueryKind::file_handlers, target);
    return;
  }

  const bool get_app = g_str_equal(method, "getApp");
  const bool get_icon = g_str_equal(method, "getAppIcon");
  const bool launch = g_str_equal(method, "openApp");
  const bool source = g_str_equal(method, "getInstallSourceInfo");
  const bool metadata = g_str_equal(method, "getAppMetadata");
  const bool technologies = g_str_equal(method, "detectAppTechnologies");
  if (!get_app && !get_icon && !launch && !source && !metadata && !technologies) {
    fl_method_call_respond_not_implemented(call, nullptr);
    return;
  }
  if (target == nullptr || !g_str_has_suffix(target, ".desktop") ||
      g_strstr_len(target, -1, "/") != nullptr) {
    fl_method_call_respond_error(call, "ARG", "Expected a desktop file ID",
                                 nullptr, nullptr);
    return;
  }
  if (technologies) {
    schedule_query(call, QueryKind::elf, target);
    return;
  }

  g_autoptr(GDesktopAppInfo) desktop = g_desktop_app_info_new(target);
  g_autoptr(FlValue) response = nullptr;
  if (desktop == nullptr) {
    if (launch) response = fl_value_new_bool(false);
  } else if (launch) {
    open_app(call, G_APP_INFO(desktop));
    return;
  } else if (get_icon) {
    response = app_icon(G_APP_INFO(desktop));
  } else if (source) {
    response = install_source_value(desktop);
  } else if (metadata) {
    response = appstream_locations(desktop);
  } else {
    response = app_value(G_APP_INFO(desktop), true);
  }
  fl_method_call_respond_success(call, response, nullptr);
}

static std::unique_ptr<DiscoveryPluginContext> create_discovery_context() {
  auto context = std::make_unique<DiscoveryPluginContext>();
  context->service = std::make_shared<
      flutter_device_apps_linux::ApplicationDiscoveryService>();
  context->service->register_provider(
      std::make_unique<flutter_device_apps_linux::GioDiscoveryProvider>());
  context->service->register_provider(
      std::make_unique<flutter_device_apps_linux::FlatpakDiscoveryProvider>(
          flutter_device_apps_linux::create_libflatpak_backend()));
  context->service->register_provider(
      std::make_unique<flutter_device_apps_linux::SnapDiscoveryProvider>(
          flutter_device_apps_linux::create_snapd_socket_backend()));
  context->service->register_package_provider(
      std::make_unique<flutter_device_apps_linux::DpkgDiscoveryProvider>());
  context->service->register_package_provider(
      std::make_unique<flutter_device_apps_linux::RpmDiscoveryProvider>(
          flutter_device_apps_linux::create_librpm_backend()));
  context->service->register_package_provider(
      std::make_unique<flutter_device_apps_linux::PacmanDiscoveryProvider>(
          flutter_device_apps_linux::create_libalpm_backend()));
  context->service->set_metadata_provider(
      std::make_unique<flutter_device_apps_linux::AppStreamMetadataProvider>(
          flutter_device_apps_linux::create_libappstream_backend()));

  std::shared_ptr<GMainContext> platform_context(
      g_main_context_ref_thread_default(), g_main_context_unref);

  auto schedule_worker = [](std::function<void()> work) {
    auto* task_work = new std::function<void()>(std::move(work));
    g_autoptr(GTask) task = g_task_new(nullptr, nullptr, nullptr, nullptr);
    g_task_set_task_data(task, task_work, [](gpointer data) {
      delete static_cast<std::function<void()>*>(data);
    });
    g_task_run_in_thread(
        task, [](GTask*, gpointer, gpointer data, GCancellable*) {
          auto* fn = static_cast<std::function<void()>*>(data);
          try {
            if (fn != nullptr && *fn) {
              (*fn)();
            }
          } catch (...) {
            // Never allow C++ exceptions to cross GLib C callback boundaries.
          }
        });
  };

  // Explicitly attach a GSource to platform_context rather than calling
  // g_main_context_invoke_full(). g_main_context_invoke_full() may acquire an
  // unowned default GMainContext and invoke the callback synchronously on the
  // worker thread; an attached idle GSource only runs on the thread iterating
  // platform_context (the Flutter Linux platform main loop).
  auto dispatch_completion = [platform_context](
                                 std::function<void()> completion) {
    auto* fn = new std::function<void()>(std::move(completion));
    GSource* source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(
        source,
        [](gpointer data) -> gboolean {
          auto* callback = static_cast<std::function<void()>*>(data);
          try {
            if (callback != nullptr && *callback) {
              (*callback)();
            }
          } catch (...) {
            // Never allow C++ exceptions to cross GLib C callback boundaries.
          }
          return G_SOURCE_REMOVE;
        },
        fn,
        [](gpointer data) {
          delete static_cast<std::function<void()>*>(data);
        });
    g_source_attach(source, platform_context.get());
    g_source_unref(source);
  };

  context->runner =
      std::make_unique<flutter_device_apps_linux::AsyncDiscoveryRunner>(
          context->service, std::move(schedule_worker),
          std::move(dispatch_completion));
  return context;
}

void flutter_device_apps_linux_plugin_register_with_registrar(
    FlPluginRegistrar* registrar) {
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar),
      "flutter_device_apps_linux/methods", FL_METHOD_CODEC(codec));
  auto discovery_context = create_discovery_context();
  fl_method_channel_set_method_call_handler(
      channel, handle_method_call, discovery_context.release(),
      [](gpointer data) {
        delete static_cast<DiscoveryPluginContext*>(data);
      });
}
