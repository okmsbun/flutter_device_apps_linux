#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "discovery/application_discovery_provider.h"
#include "discovery/application_discovery_service.h"
#include "discovery/application_identity.h"
#include "discovery/async_discovery_runner.h"
#include "discovery/discovered_application.h"
#include "discovery/flatpak_discovery_provider.h"
#include "discovery/legacy_list_apps_adapter.h"
#include "discovery/snap_discovery_provider.h"
#include "discovery/snapd_socket_backend.h"

namespace {

using flutter_device_apps_linux::ApplicationClassification;
using flutter_device_apps_linux::ApplicationDiscoveryProvider;
using flutter_device_apps_linux::ApplicationDiscoveryService;
using flutter_device_apps_linux::AsyncDiscoveryRunner;
using flutter_device_apps_linux::build_application_id;
using flutter_device_apps_linux::capability_from_bool;
using flutter_device_apps_linux::CapabilityState;
using flutter_device_apps_linux::create_snapd_socket_backend;
using flutter_device_apps_linux::DesktopMenuVisibility;
using flutter_device_apps_linux::DiscoveredApplication;
using flutter_device_apps_linux::DiscoveryExecutionOutcome;
using flutter_device_apps_linux::DiscoveryExecutionStatus;
using flutter_device_apps_linux::DiscoveryPassResult;
using flutter_device_apps_linux::DiscoveryRequestHandle;
using flutter_device_apps_linux::DiscoveryRequestOptions;
using flutter_device_apps_linux::DiscoveryResult;
using flutter_device_apps_linux::FlatpakBackend;
using flutter_device_apps_linux::FlatpakBackendQueryResult;
using flutter_device_apps_linux::FlatpakDiscoveryProvider;
using flutter_device_apps_linux::FlatpakInstallationQueryResult;
using flutter_device_apps_linux::FlatpakInstalledAppRecord;
using flutter_device_apps_linux::InstallationScope;
using flutter_device_apps_linux::InstallationSource;
using flutter_device_apps_linux::is_valid_legacy_desktop_file_id;
using flutter_device_apps_linux::kFlatpakRefKindApp;
using flutter_device_apps_linux::kFlatpakRefKindRuntime;
using flutter_device_apps_linux::OptionalString;
using flutter_device_apps_linux::ProviderExecutionStatus;
using flutter_device_apps_linux::SnapBackend;
using flutter_device_apps_linux::SnapBackendQueryResult;
using flutter_device_apps_linux::SnapDiscoveryProvider;
using flutter_device_apps_linux::SnapdSocketConfig;
using flutter_device_apps_linux::SnapInstalledRecord;
using flutter_device_apps_linux::SnapQueryErrorKind;

std::mutex g_failure_mutex;
int g_failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::lock_guard<std::mutex> lock(g_failure_mutex);
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

class Latch {
 public:
  explicit Latch(int count = 1) : remaining_(count) {}

  void count_down() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (remaining_ > 0) {
      --remaining_;
      if (remaining_ == 0) {
        cv_.notify_all();
      }
    }
  }

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() { return remaining_ == 0; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  int remaining_;
};

// Deterministic single-threaded completion pump standing in for the platform
// GMainContext during pure C++14 async tests.
class TestPlatformLoop {
 public:
  void post(std::function<void()> callback) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callbacks_.push(std::move(callback));
    }
    cv_.notify_one();
  }

  void run_one() {
    std::function<void()> callback;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() { return !callbacks_.empty(); });
      callback = std::move(callbacks_.front());
      callbacks_.pop();
    }
    if (callback) callback();
  }

  void run_until(int expected_count) {
    for (int i = 0; i < expected_count; ++i) {
      run_one();
    }
  }

  void drain_available() {
    while (true) {
      std::function<void()> callback;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (callbacks_.empty()) return;
        callback = std::move(callbacks_.front());
        callbacks_.pop();
      }
      if (callback) callback();
    }
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> callbacks_;
};

class WorkerPoolFixture {
 public:
  ~WorkerPoolFixture() { join_all(); }

  AsyncDiscoveryRunner::WorkScheduler scheduler() {
    return [this](std::function<void()> work) {
      ++scheduled_workers_;
      std::lock_guard<std::mutex> lock(mutex_);
      threads_.emplace_back(std::move(work));
    };
  }

  void join_all() {
    std::vector<std::thread> to_join;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      to_join.swap(threads_);
    }
    for (auto& thread : to_join) {
      if (thread.joinable()) thread.join();
    }
  }

  int scheduled_workers() const { return scheduled_workers_.load(); }

 private:
  std::mutex mutex_;
  std::vector<std::thread> threads_;
  std::atomic<int> scheduled_workers_{0};
};

class FakeDiscoveryProvider final : public ApplicationDiscoveryProvider {
 public:
  FakeDiscoveryProvider(std::string id, bool available, DiscoveryResult result,
                        int* discover_calls = nullptr,
                        int* destroyed_count = nullptr)
      : id_(std::move(id)),
        available_(available),
        result_(std::move(result)),
        discover_calls_(discover_calls),
        destroyed_count_(destroyed_count) {}

  ~FakeDiscoveryProvider() override {
    if (destroyed_count_ != nullptr) {
      ++(*destroyed_count_);
    }
  }

  const char* id() const override { return id_.c_str(); }

  bool is_available() const override { return available_; }

  DiscoveryResult discover() override {
    if (discover_calls_ != nullptr) {
      ++(*discover_calls_);
    }
    return result_;
  }

 private:
  std::string id_;
  bool available_;
  DiscoveryResult result_;
  int* discover_calls_;
  int* destroyed_count_;
};

class ControlledDiscoveryProvider final : public ApplicationDiscoveryProvider {
 public:
  ControlledDiscoveryProvider(std::string id,
                              std::function<DiscoveryResult()> on_discover,
                              std::atomic<int>* destroyed_count = nullptr)
      : id_(std::move(id)),
        on_discover_(std::move(on_discover)),
        destroyed_count_(destroyed_count) {}

  ~ControlledDiscoveryProvider() override {
    if (destroyed_count_ != nullptr) {
      destroyed_count_->fetch_add(1);
    }
  }

  const char* id() const override { return id_.c_str(); }

  bool is_available() const override { return true; }

  DiscoveryResult discover() override { return on_discover_(); }

 private:
  std::string id_;
  std::function<DiscoveryResult()> on_discover_;
  std::atomic<int>* destroyed_count_;
};

DiscoveredApplication make_gio_app(
    const char* desktop_id, const char* name, bool supports_files,
    bool supports_uris, bool requires_terminal,
    DesktopMenuVisibility visibility = DesktopMenuVisibility::kVisible) {
  DiscoveredApplication app;
  app.provenance.provider_id = "gio";
  app.provenance.native_id = desktop_id;
  app.provenance.installation_source = InstallationSource::kUnknown;
  app.provenance.installation_scope = InstallationScope::kUnknown;
  app.application_id = build_application_id(
      app.provenance.provider_id, app.provenance.native_id,
      app.provenance.installation_scope, app.installation_id);
  app.desktop_file_id = OptionalString::of(desktop_id);
  app.desktop_menu_visibility = visibility;
  if (name != nullptr) {
    app.metadata.name = OptionalString::of(name);
  }
  app.capabilities.supports_files = capability_from_bool(supports_files);
  app.capabilities.supports_uris = capability_from_bool(supports_uris);
  app.capabilities.requires_terminal = capability_from_bool(requires_terminal);
  return app;
}

DiscoveredApplication make_non_desktop_app(
    const std::string& provider_id, const std::string& native_id,
    InstallationSource source, InstallationScope scope,
    const OptionalString& installation_id = OptionalString::none()) {
  DiscoveredApplication app;
  app.provenance.provider_id = provider_id;
  app.provenance.native_id = native_id;
  app.provenance.installation_source = source;
  app.provenance.installation_scope = scope;
  app.installation_id = installation_id;
  app.application_id =
      build_application_id(provider_id, native_id, scope, installation_id);
  return app;
}

// --- Unified model and visibility tests ---

void test_unified_model_defaults_are_unknown_and_missing() {
  DiscoveredApplication app;
  expect(!app.desktop_file_id.has_value(),
         "desktop_file_id should default to missing");
  expect(!app.package_id.has_value(), "package_id should default to missing");
  expect(!app.installation_id.has_value(),
         "installation_id should default to missing");
  expect(!app.metadata.name.has_value(), "name should default to missing");
  expect(!app.metadata.version.has_value(),
         "version should default to missing");
  expect(!app.metadata.description.has_value(),
         "description should default to missing");
  expect(!app.metadata.executable.has_value(),
         "executable should default to missing");
  expect(!app.metadata.homepage.has_value(),
         "homepage should default to missing");
  expect(!app.metadata.license.has_value(),
         "license should default to missing");
  expect(!app.metadata.icon_reference.has_value(),
         "icon_reference should default to missing");
  expect(!app.metadata.architecture.has_value(),
         "architecture should default to missing");

  expect(app.capabilities.supports_files == CapabilityState::kUnknown,
         "supports_files should default to kUnknown");
  expect(app.capabilities.supports_uris == CapabilityState::kUnknown,
         "supports_uris should default to kUnknown");
  expect(app.capabilities.requires_terminal == CapabilityState::kUnknown,
         "requires_terminal should default to kUnknown");
  expect(app.capabilities.launchable == CapabilityState::kUnknown,
         "launchable should default to kUnknown");

  expect(app.provenance.installation_source == InstallationSource::kUnknown,
         "installation_source should default to kUnknown");
  expect(app.provenance.installation_scope == InstallationScope::kUnknown,
         "installation_scope should default to kUnknown");
  expect(app.classification == ApplicationClassification::kUnknown,
         "classification should default to kUnknown");
  expect(app.desktop_menu_visibility == DesktopMenuVisibility::kUnknown,
         "desktop_menu_visibility should default to kUnknown");
}

void test_visibility_states_remain_independent_from_source_and_classification() {
  DiscoveredApplication visible = make_gio_app(
      "org.example.Visible.desktop", "Visible", true, false, false,
      DesktopMenuVisibility::kVisible);
  DiscoveredApplication not_visible = make_gio_app(
      "org.example.NoDisplay.desktop", "NoDisplay", false, true, true,
      DesktopMenuVisibility::kNotVisible);
  DiscoveredApplication unknown_vis = make_non_desktop_app(
      "dpkg", "ripgrep:amd64", InstallationSource::kDpkg,
      InstallationScope::kSystem);

  expect(visible.desktop_menu_visibility == DesktopMenuVisibility::kVisible,
         "visible record should store kVisible");
  expect(not_visible.desktop_menu_visibility ==
             DesktopMenuVisibility::kNotVisible,
         "non-visible record should store kNotVisible");
  expect(unknown_vis.desktop_menu_visibility == DesktopMenuVisibility::kUnknown,
         "non-desktop record should leave visibility kUnknown");

  expect(not_visible.application_id == "gio:org.example.NoDisplay.desktop",
         "non-visible record should preserve source-aware identity");
  expect(not_visible.provenance.installation_source ==
             InstallationSource::kUnknown,
         "visibility must not imply installation source");
  expect(not_visible.classification == ApplicationClassification::kUnknown,
         "visibility must not imply application classification");
  expect(visible.classification == ApplicationClassification::kUnknown,
         "visible desktop entry must not assume classification without policy");
  expect(not_visible.capabilities.launchable == CapabilityState::kUnknown,
         "visibility must not be conflated with launchability");
}

void test_unified_model_distinguishes_empty_vs_missing_and_false_vs_unknown() {
  DiscoveredApplication app = make_non_desktop_app(
      "dpkg", "ripgrep:amd64", InstallationSource::kDpkg,
      InstallationScope::kSystem);
  app.package_id = OptionalString::of("ripgrep:amd64");
  app.metadata.name = OptionalString::of("");
  app.metadata.version = OptionalString::none();
  app.capabilities.supports_files = CapabilityState::kUnsupported;
  app.capabilities.supports_uris = CapabilityState::kUnknown;
  app.capabilities.requires_terminal = CapabilityState::kSupported;

  expect(!app.desktop_file_id.has_value(),
         "non-desktop application must not fabricate desktop_file_id");
  expect(app.metadata.name.has_value() && app.metadata.name.value().empty(),
         "empty metadata value must stay distinct from missing");
  expect(!app.metadata.version.has_value(),
         "missing metadata value must have has_value() == false");
  expect(app.metadata.name != app.metadata.version,
         "OptionalString::of(\"\") must not equal OptionalString::none()");
  expect(app.capabilities.supports_files != app.capabilities.supports_uris,
         "kUnsupported must not equal kUnknown");
}

// --- Identity contract tests ---

void test_identity_contract_and_collision_resistance() {
  DiscoveredApplication english = make_gio_app(
      "org.example.Editor.desktop", "Editor", true, false, false);
  DiscoveredApplication turkish = make_gio_app(
      "org.example.Editor.desktop", "Metin Duzenleyici", true, false, false);
  expect(english.application_id == "gio:org.example.Editor.desktop",
         "GIO application ID should use provider:native encoding");
  expect(english.application_id == turkish.application_id,
         "application_id must not depend on localized display name");

  expect(build_application_id("flatpak", "org.example.Editor") !=
             build_application_id("appstream", "org.example.Editor"),
         "same native ID from different providers must stay distinct");
  expect(build_application_id("flatpak", "org.example.Editor",
                              InstallationScope::kSystem) !=
             build_application_id("flatpak", "org.example.Editor",
                                  InstallationScope::kUser),
         "system and user scopes must produce distinct identities");
  expect(build_application_id("flatpak", "org.example.Editor",
                              InstallationScope::kUser) !=
             build_application_id("flatpak", "org.example.Editor",
                                  InstallationScope::kUser,
                                  OptionalString::of("")),
         "missing installation_id must not collide with empty installation_id");

  expect(build_application_id("a:b", "c") != build_application_id("a", "b:c"),
         "colon in provider_id vs native_id must not collide");
  expect(build_application_id("a:b", "c") !=
             build_application_id("a%3Ab", "c"),
         "literal %3A must be escaped so it cannot collide with encoded colon");
  expect(build_application_id("p", "n:user") !=
             build_application_id("p", "n", InstallationScope::kUser),
         "colon suffix in native_id must not collide with installation scope");
  expect(build_application_id("p", std::string("a\0b", 3)) !=
             build_application_id("p", "a%00b"),
         "embedded NUL byte must encode without collision");
}

// --- Legacy adapter tests ---

void test_legacy_desktop_file_id_validation() {
  expect(is_valid_legacy_desktop_file_id(
             OptionalString::of("org.example.Editor.desktop")),
         "standard desktop file ID should be valid");
  expect(!is_valid_legacy_desktop_file_id(OptionalString::none()),
         "missing desktop file ID must be rejected");
  expect(!is_valid_legacy_desktop_file_id(OptionalString::of("")),
         "empty desktop file ID must be rejected");
  expect(!is_valid_legacy_desktop_file_id(OptionalString::of(".desktop")),
         "bare .desktop suffix must be rejected");
  expect(!is_valid_legacy_desktop_file_id(OptionalString::of("org.example.App")),
         "ID without .desktop suffix must be rejected");
  expect(!is_valid_legacy_desktop_file_id(
             OptionalString::of("sub/org.example.App.desktop")),
         "ID containing slash must be rejected");
  expect(!is_valid_legacy_desktop_file_id(
             OptionalString::of(std::string("bad\0id.desktop", 14))),
         "ID containing NUL byte must be rejected");
}

void test_legacy_adapter_enforces_gio_visibility_and_preserves_order() {
  std::vector<DiscoveredApplication> unified;

  // 1. Visible GIO entry -> INCLUDED.
  unified.push_back(make_gio_app("org.example.First.desktop", "First", true,
                                 false, true, DesktopMenuVisibility::kVisible));

  // 2. Non-visible GIO entry (NoDisplay / OnlyShowIn / NotShowIn) -> EXCLUDED.
  unified.push_back(make_gio_app("org.example.NoDisplay.desktop", "Hidden",
                                 true, true, false,
                                 DesktopMenuVisibility::kNotVisible));

  // 3. Unknown visibility GIO entry -> EXCLUDED (never treated as visible).
  unified.push_back(make_gio_app("org.example.UnknownVis.desktop", "UnknownVis",
                                 false, false, false,
                                 DesktopMenuVisibility::kUnknown));

  // 4. Non-GIO provider record WITH valid desktop_file_id and kVisible ->
  // EXCLUDED so future providers cannot accidentally alter legacy listApps().
  DiscoveredApplication flatpak_visible = make_non_desktop_app(
      "flatpak", "org.example.FlatpakApp", InstallationSource::kFlatpak,
      InstallationScope::kUser);
  flatpak_visible.desktop_file_id =
      OptionalString::of("org.example.FlatpakApp.desktop");
  flatpak_visible.desktop_menu_visibility = DesktopMenuVisibility::kVisible;
  flatpak_visible.metadata.name = OptionalString::of("Flatpak App");
  unified.push_back(std::move(flatpak_visible));

  // 5. Non-desktop record -> EXCLUDED.
  unified.push_back(make_non_desktop_app("dpkg", "curl:amd64",
                                         InstallationSource::kDpkg,
                                         InstallationScope::kSystem));

  // 6. Visible GIO entry with missing app_name (null in legacy map) and
  // unknown capability state -> INCLUDED with null app_name and false bool.
  DiscoveredApplication unnamed = make_gio_app(
      "org.example.Unnamed.desktop", nullptr, false, true, false,
      DesktopMenuVisibility::kVisible);
  unnamed.capabilities.supports_files = CapabilityState::kUnknown;
  unified.push_back(std::move(unnamed));

  // 7. Visible GIO entry with explicitly empty app_name -> INCLUDED.
  unified.push_back(make_gio_app("org.example.EmptyName.desktop", "", false,
                                 false, false,
                                 DesktopMenuVisibility::kVisible));

  // 8. Visible GIO entry with invalid desktop ID -> EXCLUDED.
  DiscoveredApplication invalid_id = make_gio_app(
      "org.example.Valid.desktop", "Bad", false, false, false,
      DesktopMenuVisibility::kVisible);
  invalid_id.desktop_file_id = OptionalString::of("/usr/share/bad.desktop");
  unified.push_back(std::move(invalid_id));

  const auto legacy =
      flutter_device_apps_linux::adapt_for_legacy_list_apps(unified);
  expect(legacy.size() == 3,
         "legacy adapter must include only visible, valid GIO records");
  if (legacy.size() == 3) {
    expect(legacy[0].desktop_file_id == "org.example.First.desktop",
           "first legacy record order should be preserved");
    expect(legacy[0].app_name.has_value() &&
               legacy[0].app_name.value() == "First",
           "first app_name should be preserved");
    expect(legacy[0].supports_files && !legacy[0].supports_uris &&
               legacy[0].requires_terminal,
           "first capability booleans should match");

    expect(legacy[1].desktop_file_id == "org.example.Unnamed.desktop",
           "second legacy record order should be preserved");
    expect(!legacy[1].app_name.has_value(),
           "missing app_name must stay unset");
    expect(!legacy[1].supports_files && legacy[1].supports_uris &&
               !legacy[1].requires_terminal,
           "unknown capability should map to false while known true stays true");

    expect(legacy[2].desktop_file_id == "org.example.EmptyName.desktop",
           "third legacy record order should be preserved");
    expect(legacy[2].app_name.has_value() && legacy[2].app_name.value().empty(),
           "empty app_name must stay present as empty string");
  }
}

// --- Coordinator and error distinction tests ---

void test_coordinator_report_distinguishes_success_unavailable_and_failure() {
  DiscoveryResult failing;
  failing.ok = false;
  failing.error_message = "db locked";
  failing.applications.push_back(make_non_desktop_app(
      "rpm", "partial", InstallationSource::kRpm, InstallationScope::kSystem));

  ApplicationDiscoveryService service;
  service.register_provider(nullptr);
  service.register_provider(
      std::make_unique<FakeDiscoveryProvider>("gio", true, DiscoveryResult{}));
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "snap", false, DiscoveryResult{}));
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "rpm", true, std::move(failing)));

  const DiscoveryPassResult report = service.discover_with_report();
  expect(report.applications.empty(),
         "failed provider partial payload must not leak into results");
  expect(report.provider_summaries.size() == 3,
         "all registered providers should be summarized");
  if (report.provider_summaries.size() == 3) {
    expect(report.provider_summaries[0].provider_id == "gio" &&
               report.provider_summaries[0].status ==
                   ProviderExecutionStatus::kSucceeded &&
               report.provider_summaries[0].discovered_count == 0,
           "empty healthy provider should report kSucceeded with count 0");
    expect(report.provider_summaries[1].provider_id == "snap" &&
               report.provider_summaries[1].status ==
                   ProviderExecutionStatus::kUnavailable,
           "unavailable provider should report kUnavailable");
    expect(report.provider_summaries[2].provider_id == "rpm" &&
               report.provider_summaries[2].status ==
                   ProviderExecutionStatus::kFailed &&
               report.provider_summaries[2].error_message == "db locked",
           "failed provider should report kFailed with error_message");
  }
}

// --- Asynchronous execution, concurrency, and lifecycle tests ---

void test_async_completion_context_empty_and_provider_failures() {
  auto service = std::make_shared<ApplicationDiscoveryService>();
  service->register_provider(
      std::make_unique<FakeDiscoveryProvider>("gio", true, DiscoveryResult{}));
  DiscoveryResult failed;
  failed.ok = false;
  failed.error_message = "simulated failure";
  service->register_provider(std::make_unique<FakeDiscoveryProvider>(
      "flatpak", true, std::move(failed)));

  TestPlatformLoop loop;
  WorkerPoolFixture pool;
  const std::thread::id platform_thread = std::this_thread::get_id();
  std::thread::id completion_thread;
  int callback_count = 0;
  DiscoveryExecutionOutcome received;

  AsyncDiscoveryRunner runner(
      service, pool.scheduler(),
      [&loop](std::function<void()> cb) { loop.post(std::move(cb)); });

  runner.discover_async([&](DiscoveryExecutionOutcome outcome) {
    completion_thread = std::this_thread::get_id();
    ++callback_count;
    received = std::move(outcome);
  });

  loop.run_one();
  pool.join_all();

  expect(callback_count == 1, "completion callback must run exactly once");
  expect(completion_thread == platform_thread,
         "completion must execute on the platform thread");
  expect(received.status == DiscoveryExecutionStatus::kCompleted,
         "discovery pass with empty + isolated failure should complete");
  expect(received.applications.empty(), "applications should be empty");
  expect(received.provider_summaries.size() == 2 &&
             received.provider_summaries[1].status ==
                 ProviderExecutionStatus::kFailed,
         "provider failure summary should be preserved");
}

void test_async_worker_exception_produces_execution_error() {
  auto service = std::make_shared<ApplicationDiscoveryService>();
  service->register_provider(std::make_unique<ControlledDiscoveryProvider>(
      "throwing",
      []() -> DiscoveryResult { throw std::runtime_error("boom"); }));

  TestPlatformLoop loop;
  WorkerPoolFixture pool;
  DiscoveryExecutionOutcome received;

  AsyncDiscoveryRunner runner(
      service, pool.scheduler(),
      [&loop](std::function<void()> cb) { loop.post(std::move(cb)); });

  runner.discover_async([&](DiscoveryExecutionOutcome outcome) {
    received = std::move(outcome);
  });

  loop.run_one();
  pool.join_all();

  expect(received.status == DiscoveryExecutionStatus::kExecutionError,
         "unexpected worker exception should report kExecutionError");
  expect(!received.error_message.empty(),
         "execution error should populate internal error_message");
}

void test_async_serializes_overlapping_requests_and_keeps_platform_responsive() {
  Latch first_started(1);
  Latch release_first(1);
  std::atomic<int> in_flight{0};
  std::atomic<int> max_in_flight{0};
  std::atomic<int> call_index{0};

  auto service = std::make_shared<ApplicationDiscoveryService>();
  service->register_provider(std::make_unique<ControlledDiscoveryProvider>(
      "gio", [&]() {
        const int current = in_flight.fetch_add(1) + 1;
        int prev_max = max_in_flight.load();
        while (current > prev_max &&
               !max_in_flight.compare_exchange_weak(prev_max, current)) {
        }

        const int invocation = call_index.fetch_add(1) + 1;
        if (invocation == 1) {
          first_started.count_down();
          release_first.wait();
        }

        DiscoveryResult result;
        const std::string id =
            "org.example.Pass" + std::to_string(invocation) + ".desktop";
        result.applications.push_back(
            make_gio_app(id.c_str(), "Pass", true, false, false));
        in_flight.fetch_sub(1);
        return result;
      }));

  TestPlatformLoop loop;
  WorkerPoolFixture pool;
  AsyncDiscoveryRunner runner(
      service, pool.scheduler(),
      [&loop](std::function<void()> cb) { loop.post(std::move(cb)); });

  std::vector<int> completion_order;
  std::vector<std::string> app_ids;

  // Submit first (slow) request; platform thread returns immediately.
  runner.discover_async([&](DiscoveryExecutionOutcome outcome) {
    expect(outcome.status == DiscoveryExecutionStatus::kCompleted,
           "request 1 should complete");
    completion_order.push_back(1);
    if (!outcome.applications.empty()) {
      app_ids.push_back(outcome.applications.front().desktop_file_id.value());
    }
  });

  // Wait deterministically until worker is actively blocked inside request 1.
  first_started.wait();

  // Platform thread remains responsive while request 1 is blocked and can
  // enqueue additional overlapping requests without blocking or spawning extra
  // workers.
  runner.discover_async([&](DiscoveryExecutionOutcome outcome) {
    expect(outcome.status == DiscoveryExecutionStatus::kCompleted,
           "request 2 should complete");
    completion_order.push_back(2);
    if (!outcome.applications.empty()) {
      app_ids.push_back(outcome.applications.front().desktop_file_id.value());
    }
  });
  runner.discover_async([&](DiscoveryExecutionOutcome outcome) {
    expect(outcome.status == DiscoveryExecutionStatus::kCompleted,
           "request 3 should complete");
    completion_order.push_back(3);
    if (!outcome.applications.empty()) {
      app_ids.push_back(outcome.applications.front().desktop_file_id.value());
    }
  });

  expect(pool.scheduled_workers() == 1,
         "overlapping requests must reuse a single active worker");

  // Unblock the slow provider and pump all 3 completions on the platform loop.
  release_first.count_down();
  loop.run_until(3);
  pool.join_all();

  expect(max_in_flight.load() == 1,
         "provider must never be accessed concurrently");
  expect(call_index.load() == 3, "all 3 requests must execute without drops");
  expect(completion_order.size() == 3 && completion_order[0] == 1 &&
             completion_order[1] == 2 && completion_order[2] == 3,
         "overlapping requests must complete in FIFO order");
  expect(app_ids.size() == 3 &&
             app_ids[0] == "org.example.Pass1.desktop" &&
             app_ids[1] == "org.example.Pass2.desktop" &&
             app_ids[2] == "org.example.Pass3.desktop",
         "each request must receive its own discovery pass results");
}

void test_async_cancellation_for_queued_and_in_flight_requests() {
  Latch first_started(1);
  Latch release_first(1);
  std::atomic<int> provider_calls{0};

  auto service = std::make_shared<ApplicationDiscoveryService>();
  service->register_provider(std::make_unique<ControlledDiscoveryProvider>(
      "gio", [&]() {
        const int n = provider_calls.fetch_add(1) + 1;
        if (n == 1) {
          first_started.count_down();
          release_first.wait();
        }
        DiscoveryResult result;
        result.applications.push_back(make_gio_app(
            "org.example.App.desktop", "App", false, false, false));
        return result;
      }));

  TestPlatformLoop loop;
  WorkerPoolFixture pool;
  AsyncDiscoveryRunner runner(
      service, pool.scheduler(),
      [&loop](std::function<void()> cb) { loop.post(std::move(cb)); });

  DiscoveryExecutionStatus first_status = DiscoveryExecutionStatus::kCompleted;
  DiscoveryExecutionStatus second_status = DiscoveryExecutionStatus::kCompleted;
  DiscoveryExecutionStatus third_status = DiscoveryExecutionStatus::kCancelled;

  DiscoveryRequestHandle first_handle =
      runner.discover_async([&](DiscoveryExecutionOutcome outcome) {
        first_status = outcome.status;
      });
  first_started.wait();

  DiscoveryRequestHandle second_handle =
      runner.discover_async([&](DiscoveryExecutionOutcome outcome) {
        second_status = outcome.status;
      });
  runner.discover_async([&](DiscoveryExecutionOutcome outcome) {
    third_status = outcome.status;
  });

  // Cancel both the in-flight request (1) and the queued request (2).
  first_handle.cancel();
  second_handle.cancel();

  release_first.count_down();
  loop.run_until(3);
  pool.join_all();

  expect(first_status == DiscoveryExecutionStatus::kCancelled,
         "cancelled in-flight request should finish as kCancelled");
  expect(second_status == DiscoveryExecutionStatus::kCancelled,
         "cancelled queued request should finish as kCancelled");
  expect(third_status == DiscoveryExecutionStatus::kCompleted,
         "subsequent non-cancelled request should still complete");
  expect(provider_calls.load() == 2,
         "cancelled queued request must not invoke provider discover()");
}

void test_teardown_while_discovery_pending_is_non_blocking_and_safe() {
  Latch slow_started(1);
  Latch release_slow(1);
  std::atomic<int> provider_destroyed{0};
  std::atomic<int> first_completions{0};
  std::atomic<int> second_completions{0};
  DiscoveryExecutionStatus first_status = DiscoveryExecutionStatus::kCompleted;
  DiscoveryExecutionStatus second_status = DiscoveryExecutionStatus::kCompleted;

  TestPlatformLoop loop;
  WorkerPoolFixture pool;

  {
    auto service = std::make_shared<ApplicationDiscoveryService>();
    service->register_provider(std::make_unique<ControlledDiscoveryProvider>(
        "gio",
        [&]() {
          slow_started.count_down();
          release_slow.wait();
          DiscoveryResult result;
          result.applications.push_back(make_gio_app(
              "org.example.Slow.desktop", "Slow", true, false, false));
          return result;
        },
        &provider_destroyed));

    auto runner = std::make_unique<AsyncDiscoveryRunner>(
        service, pool.scheduler(),
        [&loop](std::function<void()> cb) { loop.post(std::move(cb)); });

    // Drop local service ref so only runner/worker keep it alive.
    service.reset();

    runner->discover_async([&](DiscoveryExecutionOutcome outcome) {
      first_completions.fetch_add(1);
      first_status = outcome.status;
    });
    slow_started.wait();

    runner->discover_async([&](DiscoveryExecutionOutcome outcome) {
      second_completions.fetch_add(1);
      second_status = outcome.status;
    });

    // Simulate plugin teardown while request 1 is still blocked on worker and
    // request 2 is queued. Destroying runner calls shutdown() and must NOT
    // block waiting for release_slow!
    runner.reset();

    expect(second_completions.load() == 1 &&
               second_status == DiscoveryExecutionStatus::kCancelled,
           "queued request must be cancelled immediately during teardown");
    expect(provider_destroyed.load() == 0,
           "provider and service must stay alive while slow worker is running");
    expect(first_completions.load() == 0,
           "in-flight request has not finished yet");

    // Now allow the slow worker to exit.
    release_slow.count_down();
  }

  pool.join_all();
  loop.drain_available();

  expect(first_completions.load() == 1 &&
             first_status == DiscoveryExecutionStatus::kCancelled,
         "in-flight request during teardown must deliver kCancelled once");
  expect(provider_destroyed.load() == 1,
         "service and provider must be deterministically destroyed once worker "
         "and completion finish");
}

void test_teardown_with_stopped_platform_loop_releases_pending_calls() {
  struct PlatformOnlyCall {
    std::thread::id owner_thread;
    int* released_count;
    bool* wrong_thread_release;

    ~PlatformOnlyCall() {
      if (std::this_thread::get_id() != owner_thread) {
        *wrong_thread_release = true;
      }
      ++(*released_count);
    }
  };

  struct PlatformCallTable {
    bool torn_down = false;
    uint64_t next_id = 0;
    std::unordered_map<uint64_t, std::unique_ptr<PlatformOnlyCall>> calls;

    uint64_t add(std::unique_ptr<PlatformOnlyCall> call) {
      const uint64_t id = ++next_id;
      calls.emplace(id, std::move(call));
      return id;
    }

    std::unique_ptr<PlatformOnlyCall> take(uint64_t id) {
      if (torn_down) return nullptr;
      const auto it = calls.find(id);
      if (it == calls.end()) return nullptr;
      auto call = std::move(it->second);
      calls.erase(it);
      return call;
    }

    void teardown() {
      torn_down = true;
      calls.clear();
    }
  };

  const std::thread::id platform_thread = std::this_thread::get_id();
  int released_calls = 0;
  bool wrong_thread_release = false;
  std::atomic<int> responses{0};
  Latch slow_started(1);
  Latch release_slow(1);

  std::mutex stopped_loop_mutex;
  std::vector<std::function<void()>> undelivered_callbacks;
  WorkerPoolFixture pool;

  {
    auto service = std::make_shared<ApplicationDiscoveryService>();
    service->register_provider(std::make_unique<ControlledDiscoveryProvider>(
        "gio", [&]() {
          slow_started.count_down();
          release_slow.wait();
          DiscoveryResult result;
          result.applications.push_back(make_gio_app(
              "org.example.Slow.desktop", "Slow", true, false, false));
          return result;
        }));

    auto table = std::make_shared<PlatformCallTable>();
    AsyncDiscoveryRunner runner(
        service, pool.scheduler(), [&](std::function<void()> cb) {
          // Simulate a stopped main loop: callbacks are enqueued but never
          // dispatched before teardown/destruction.
          std::lock_guard<std::mutex> lock(stopped_loop_mutex);
          undelivered_callbacks.push_back(std::move(cb));
        });

    const uint64_t first_id = table->add(std::unique_ptr<PlatformOnlyCall>(
        new PlatformOnlyCall{platform_thread, &released_calls,
                             &wrong_thread_release}));
    std::weak_ptr<PlatformCallTable> weak_table(table);
    runner.discover_async([first_id, weak_table, &responses](
                              DiscoveryExecutionOutcome) {
      auto locked = weak_table.lock();
      if (locked == nullptr) return;
      auto call = locked->take(first_id);
      if (call != nullptr) responses.fetch_add(1);
    });
    slow_started.wait();

    const uint64_t second_id = table->add(std::unique_ptr<PlatformOnlyCall>(
        new PlatformOnlyCall{platform_thread, &released_calls,
                             &wrong_thread_release}));
    runner.discover_async([second_id, weak_table, &responses](
                              DiscoveryExecutionOutcome) {
      auto locked = weak_table.lock();
      if (locked == nullptr) return;
      auto call = locked->take(second_id);
      if (call != nullptr) responses.fetch_add(1);
    });

    // Teardown on platform thread while slow worker is still running and main
    // loop will never pump again.
    table->teardown();
    runner.shutdown();
    table.reset();

    expect(released_calls == 2,
           "all pending calls (in-flight and queued) must release immediately "
           "on platform thread during teardown");
    expect(!wrong_thread_release,
           "platform call handles must never release on a worker thread");

    release_slow.count_down();
  }

  pool.join_all();
  // Destroy un-dispatched callbacks on a background thread to simulate
  // GMainContext finalization from a worker thread after main loop stop.
  std::thread cleanup([&]() {
    std::lock_guard<std::mutex> lock(stopped_loop_mutex);
    undelivered_callbacks.clear();
  });
  cleanup.join();

  expect(responses.load() == 0,
         "no response must be sent after teardown with stopped loop");
  expect(!wrong_thread_release,
         "destroying undelivered callbacks on another thread must not touch "
         "platform call handles");
}

void test_concurrent_registration_and_discovery_on_service() {
  ApplicationDiscoveryService service;
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "gio", true, DiscoveryResult{}));

  std::thread reader([&]() {
    for (int i = 0; i < 20; ++i) {
      (void)service.discover_applications();
    }
  });
  std::thread writer([&]() {
    for (int i = 0; i < 5; ++i) {
      service.register_provider(std::make_unique<FakeDiscoveryProvider>(
          "extra", true, DiscoveryResult{}));
    }
  });

  reader.join();
  writer.join();
  expect(service.provider_count() == 6,
         "concurrent registration and discovery must not race");
}

// --- FlatpakDiscoveryProvider tests ---

class FakeFlatpakBackend final : public FlatpakBackend {
 public:
  FakeFlatpakBackend(bool available, FlatpakBackendQueryResult result,
                     int* list_calls = nullptr, int* destroyed_count = nullptr)
      : available_(available),
        result_(std::move(result)),
        list_calls_(list_calls),
        destroyed_count_(destroyed_count) {}

  ~FakeFlatpakBackend() override {
    if (destroyed_count_ != nullptr) {
      ++(*destroyed_count_);
    }
  }

  bool is_available() const override { return available_; }

  FlatpakBackendQueryResult list_installed_apps() override {
    if (list_calls_ != nullptr) {
      ++(*list_calls_);
    }
    return result_;
  }

 private:
  bool available_;
  FlatpakBackendQueryResult result_;
  int* list_calls_;
  int* destroyed_count_;
};

FlatpakInstalledAppRecord make_flatpak_ref(
    const std::string& app_id, const std::string& branch,
    const std::string& arch, int kind = kFlatpakRefKindApp,
    const OptionalString& name = OptionalString::none(),
    const OptionalString& version = OptionalString::none(),
    const OptionalString& summary = OptionalString::none()) {
  FlatpakInstalledAppRecord ref;
  ref.app_id = app_id;
  ref.branch = branch;
  ref.arch = arch;
  ref.ref_kind = kind;
  ref.name = name;
  ref.version = version;
  ref.summary = summary;
  return ref;
}

void test_flatpak_missing_backend_or_library_is_unavailable() {
  FlatpakDiscoveryProvider null_provider(nullptr);
  expect(std::string(null_provider.id()) == "flatpak",
         "FlatpakDiscoveryProvider id() must be flatpak");
  expect(!null_provider.is_available(),
         "null backend must report unavailable");
  const DiscoveryResult null_result = null_provider.discover();
  expect(!null_result.ok, "discover() on unavailable backend must not succeed");

  int list_calls = 0;
  ApplicationDiscoveryService service;
  service.register_provider(std::make_unique<FlatpakDiscoveryProvider>(
      std::make_unique<FakeFlatpakBackend>(false, FlatpakBackendQueryResult{},
                                           &list_calls)));
  const DiscoveryPassResult pass = service.discover_with_report();
  expect(list_calls == 0,
         "service must not invoke list_installed_apps() when libflatpak missing");
  expect(pass.applications.empty(), "unavailable flatpak yields no apps");
  expect(pass.provider_summaries.size() == 1 &&
             pass.provider_summaries[0].provider_id == "flatpak" &&
             pass.provider_summaries[0].status ==
                 ProviderExecutionStatus::kUnavailable,
         "missing libflatpak must report kUnavailable");
}

void test_flatpak_empty_installations() {
  FlatpakBackendQueryResult query;
  FlatpakInstallationQueryResult user;
  user.installation_id = "user";
  user.is_user = true;
  FlatpakInstallationQueryResult sys;
  sys.installation_id = "default";
  sys.is_user = false;
  query.installations.push_back(std::move(user));
  query.installations.push_back(std::move(sys));

  ApplicationDiscoveryService service;
  service.register_provider(std::make_unique<FlatpakDiscoveryProvider>(
      std::make_unique<FakeFlatpakBackend>(true, std::move(query))));

  const DiscoveryPassResult pass = service.discover_with_report();
  expect(pass.applications.empty(),
         "empty Flatpak installations should return 0 apps");
  expect(pass.provider_summaries.size() == 1 &&
             pass.provider_summaries[0].status ==
                 ProviderExecutionStatus::kSucceeded &&
             pass.provider_summaries[0].discovered_count == 0 &&
             pass.provider_summaries[0].error_message.empty(),
         "empty healthy installations should report kSucceeded");
}

void test_flatpak_scopes_named_installs_branches_arches_metadata_and_runtimes() {
  FlatpakBackendQueryResult query;

  // User installation with full metadata + a runtime (must be excluded) +
  // a second branch containing '/'.
  FlatpakInstallationQueryResult user;
  user.installation_id = "user";
  user.is_user = true;
  user.apps.push_back(make_flatpak_ref(
      "org.example.Editor", "stable", "x86_64", kFlatpakRefKindApp,
      OptionalString::of("Example Editor"), OptionalString::of("2.4.0"),
      OptionalString::of("Fast text editor")));
  user.apps.push_back(make_flatpak_ref(
      "org.example.Editor", "beta/nightly", "x86_64", kFlatpakRefKindApp));
  user.apps.push_back(make_flatpak_ref(
      "org.freedesktop.Platform", "23.08", "x86_64", kFlatpakRefKindRuntime,
      OptionalString::of("Platform Runtime")));
  query.installations.push_back(std::move(user));

  // Default system installation: same app_id with missing optional metadata
  // (as on older libflatpak without appdata symbols) and two architectures.
  FlatpakInstallationQueryResult sys_default;
  sys_default.installation_id = "default";
  sys_default.is_user = false;
  sys_default.apps.push_back(
      make_flatpak_ref("org.example.Editor", "stable", "x86_64"));
  sys_default.apps.push_back(
      make_flatpak_ref("org.example.Editor", "stable", "aarch64"));
  query.installations.push_back(std::move(sys_default));

  // Named extra system installation with the same app_id/branch/arch.
  FlatpakInstallationQueryResult sys_extra;
  sys_extra.installation_id = "extra-sys";
  sys_extra.is_user = false;
  sys_extra.apps.push_back(
      make_flatpak_ref("org.example.Editor", "stable", "x86_64"));
  query.installations.push_back(std::move(sys_extra));

  FlatpakDiscoveryProvider provider(
      std::make_unique<FakeFlatpakBackend>(true, std::move(query)));
  const DiscoveryResult result = provider.discover();

  expect(result.ok, "healthy Flatpak discovery should succeed");
  expect(result.error_message.empty(),
         "healthy Flatpak discovery should have no diagnostics");
  expect(result.applications.size() == 5,
         "runtime must be excluded while all 5 distinct app installs remain");
  if (result.applications.size() != 5) return;

  const auto& user_stable = result.applications[0];
  const auto& user_beta = result.applications[1];
  const auto& sys_x86 = result.applications[2];
  const auto& sys_arm = result.applications[3];
  const auto& extra_x86 = result.applications[4];

  // Authoritative provenance, metadata, and unknown defaults on user_stable.
  expect(user_stable.provenance.provider_id == "flatpak",
         "provider_id must be flatpak");
  expect(user_stable.provenance.native_id == "org.example.Editor",
         "native_id must match Flatpak app ID");
  expect(user_stable.provenance.installation_source ==
             InstallationSource::kFlatpak,
         "installation_source must be kFlatpak");
  expect(user_stable.provenance.installation_scope == InstallationScope::kUser,
         "user installation must have kUser scope");
  expect(user_stable.package_id.has_value() &&
             user_stable.package_id.value() == "org.example.Editor",
         "package_id must equal Flatpak app ID");
  expect(!user_stable.desktop_file_id.has_value(),
         "Flatpak provider must NOT fabricate desktop_file_id");
  expect(user_stable.classification == ApplicationClassification::kUnknown,
         "Flatpak provider must NOT assume GUI classification");
  expect(user_stable.desktop_menu_visibility == DesktopMenuVisibility::kUnknown,
         "Flatpak provider must leave desktop_menu_visibility unknown");
  expect(user_stable.capabilities.supports_files == CapabilityState::kUnknown &&
             user_stable.capabilities.supports_uris ==
                 CapabilityState::kUnknown &&
             user_stable.capabilities.requires_terminal ==
                 CapabilityState::kUnknown &&
             user_stable.capabilities.launchable == CapabilityState::kUnknown,
         "Flatpak provider must leave unsupported capabilities unknown");
  expect(user_stable.metadata.name.has_value() &&
             user_stable.metadata.name.value() == "Example Editor",
         "appdata name must be preserved");
  expect(user_stable.metadata.version.has_value() &&
             user_stable.metadata.version.value() == "2.4.0",
         "appdata version must be preserved");
  expect(user_stable.metadata.description.has_value() &&
             user_stable.metadata.description.value() == "Fast text editor",
         "appdata summary must map to metadata.description");
  expect(user_stable.metadata.architecture.has_value() &&
             user_stable.metadata.architecture.value() == "x86_64",
         "architecture must be preserved");
  expect(user_stable.metadata.branch.has_value() &&
             user_stable.metadata.branch.value() == "stable",
         "branch must be preserved");

  // Missing optional metadata remains unset on sys_x86.
  expect(sys_x86.provenance.installation_scope == InstallationScope::kSystem,
         "system installation must have kSystem scope");
  expect(!sys_x86.metadata.name.has_value() &&
             !sys_x86.metadata.version.has_value() &&
             !sys_x86.metadata.description.has_value(),
         "missing optional appdata fields must remain unset");

  // Escaping of '/' in branch and distinctness of all 5 identities.
  expect(user_beta.installation_id.has_value() &&
             user_beta.installation_id.value() == "user/beta%2Fnightly/x86_64",
         "branch containing '/' must percent-escape '/' in installation_id");

  const std::vector<std::string> ids = {
      user_stable.application_id, user_beta.application_id,
      sys_x86.application_id, sys_arm.application_id, extra_x86.application_id};
  for (size_t i = 0; i < ids.size(); ++i) {
    for (size_t j = i + 1; j < ids.size(); ++j) {
      expect(ids[i] != ids[j],
             "distinct Flatpak installations must produce unique application_ids");
    }
  }
}

void test_flatpak_partial_and_total_installation_failures() {
  // Partial failure: user installation fails, system installation succeeds.
  {
    FlatpakBackendQueryResult query;
    FlatpakInstallationQueryResult user_failed;
    user_failed.installation_id = "user";
    user_failed.is_user = true;
    user_failed.ok = false;
    user_failed.error_message = "permission denied";
    query.installations.push_back(std::move(user_failed));

    FlatpakInstallationQueryResult sys_ok;
    sys_ok.installation_id = "default";
    sys_ok.is_user = false;
    sys_ok.apps.push_back(
        make_flatpak_ref("org.example.SystemApp", "stable", "x86_64"));
    query.installations.push_back(std::move(sys_ok));

    ApplicationDiscoveryService service;
    service.register_provider(std::make_unique<FlatpakDiscoveryProvider>(
        std::make_unique<FakeFlatpakBackend>(true, std::move(query))));

    const DiscoveryPassResult pass = service.discover_with_report();
    expect(pass.applications.size() == 1 &&
               pass.applications[0].provenance.native_id ==
                   "org.example.SystemApp",
           "accessible system installation must still contribute apps when "
           "another installation fails");
    expect(pass.provider_summaries.size() == 1 &&
               pass.provider_summaries[0].status ==
                   ProviderExecutionStatus::kPartiallyFailed &&
               pass.provider_summaries[0].error_message.find("permission denied") !=
                   std::string::npos,
           "partial failure must preserve accurate provider diagnostics");
  }

  // Total failure: all installations fail -> must NOT report empty success!
  {
    FlatpakBackendQueryResult query;
    FlatpakInstallationQueryResult sys_failed;
    sys_failed.installation_id = "default";
    sys_failed.is_user = false;
    sys_failed.ok = false;
    sys_failed.error_message = "corrupt ostree repo";
    query.installations.push_back(std::move(sys_failed));

    ApplicationDiscoveryService service;
    service.register_provider(std::make_unique<FlatpakDiscoveryProvider>(
        std::make_unique<FakeFlatpakBackend>(true, std::move(query))));

    const DiscoveryPassResult pass = service.discover_with_report();
    expect(pass.applications.empty(), "total failure should return no apps");
    expect(pass.provider_summaries.size() == 1 &&
               pass.provider_summaries[0].status ==
                   ProviderExecutionStatus::kFailed &&
               pass.provider_summaries[0].error_message.find(
                   "corrupt ostree repo") != std::string::npos,
           "total installation failure must report kFailed, not empty success");
  }
}

void test_flatpak_ownership_and_legacy_list_apps_compatibility() {
  int backend_destroyed = 0;
  {
    DiscoveryResult gio_result;
    gio_result.applications.push_back(make_gio_app(
        "org.example.Visible.desktop", "Visible", true, false, false,
        DesktopMenuVisibility::kVisible));
    gio_result.applications.push_back(make_gio_app(
        "org.example.NoDisplay.desktop", "NoDisplay", false, false, false,
        DesktopMenuVisibility::kNotVisible));

    FlatpakBackendQueryResult flatpak_query;
    FlatpakInstallationQueryResult user;
    user.installation_id = "user";
    user.is_user = true;
    user.apps.push_back(
        make_flatpak_ref("org.example.NoDesktopTool", "stable", "x86_64"));
    flatpak_query.installations.push_back(std::move(user));

    ApplicationDiscoveryService service;
    service.register_provider(std::make_unique<FakeDiscoveryProvider>(
        "gio", true, std::move(gio_result)));
    service.register_provider(std::make_unique<FlatpakDiscoveryProvider>(
        std::make_unique<FakeFlatpakBackend>(true, std::move(flatpak_query),
                                             nullptr, &backend_destroyed)));

    const auto unified = service.discover_applications();
    expect(unified.size() == 3,
           "internal discovery should include both GIO and Flatpak records");

    const auto legacy =
        flutter_device_apps_linux::adapt_for_legacy_list_apps(unified);
    expect(legacy.size() == 1 &&
               legacy[0].desktop_file_id == "org.example.Visible.desktop",
           "legacy listApps() must remain strictly visible GIO apps only");
    expect(backend_destroyed == 0,
           "FlatpakBackend must remain alive while service is alive");
  }
  expect(backend_destroyed == 1,
         "FlatpakBackend must be deterministically destroyed with service");
}

// --- Provider filtering and SnapDiscoveryProvider tests ---

class CountingProvider final : public ApplicationDiscoveryProvider {
 public:
  CountingProvider(std::string id, bool available, DiscoveryResult result,
                   std::atomic<int>* availability_calls,
                   std::atomic<int>* discover_calls)
      : id_(std::move(id)),
        available_(available),
        result_(std::move(result)),
        availability_calls_(availability_calls),
        discover_calls_(discover_calls) {}

  const char* id() const override { return id_.c_str(); }

  bool is_available() const override {
    if (availability_calls_ != nullptr) {
      availability_calls_->fetch_add(1);
    }
    return available_;
  }

  DiscoveryResult discover() override {
    if (discover_calls_ != nullptr) {
      discover_calls_->fetch_add(1);
    }
    return result_;
  }

 private:
  std::string id_;
  bool available_;
  DiscoveryResult result_;
  std::atomic<int>* availability_calls_;
  std::atomic<int>* discover_calls_;
};

void test_provider_filtering_legacy_gio_only_vs_unified() {
  std::atomic<int> gio_avail{0};
  std::atomic<int> gio_disc{0};
  std::atomic<int> flatpak_avail{0};
  std::atomic<int> flatpak_disc{0};
  std::atomic<int> snap_avail{0};
  std::atomic<int> snap_disc{0};

  DiscoveryResult gio_result;
  gio_result.applications.push_back(make_gio_app(
      "org.example.Visible.desktop", "Visible", true, false, false));

  DiscoveryResult flatpak_result;
  flatpak_result.applications.push_back(make_non_desktop_app(
      "flatpak", "org.example.FlatpakApp", InstallationSource::kFlatpak,
      InstallationScope::kUser));

  DiscoveryResult snap_result;
  snap_result.applications.push_back(make_non_desktop_app(
      "snap", "blender", InstallationSource::kSnap,
      InstallationScope::kSystem));

  auto service = std::make_shared<ApplicationDiscoveryService>();
  service->register_provider(std::make_unique<CountingProvider>(
      "gio", true, std::move(gio_result), &gio_avail, &gio_disc));
  service->register_provider(std::make_unique<CountingProvider>(
      "flatpak", true, std::move(flatpak_result), &flatpak_avail,
      &flatpak_disc));
  service->register_provider(std::make_unique<CountingProvider>(
      "snap", true, std::move(snap_result), &snap_avail, &snap_disc));

  // 1. Legacy GIO-only path via AsyncDiscoveryRunner: Flatpak and Snap must
  // NOT perform availability checks or discovery.
  TestPlatformLoop loop;
  WorkerPoolFixture pool;
  AsyncDiscoveryRunner runner(
      service, pool.scheduler(),
      [&loop](std::function<void()> cb) { loop.post(std::move(cb)); });

  DiscoveryExecutionOutcome gio_only_outcome;
  runner.discover_async(
      [&](DiscoveryExecutionOutcome outcome) {
        gio_only_outcome = std::move(outcome);
      },
      DiscoveryRequestOptions::only_providers({"gio"}));
  loop.run_one();

  expect(gio_avail.load() == 1 && gio_disc.load() == 1,
         "GIO-only filter must execute GIO provider once");
  expect(flatpak_avail.load() == 0 && flatpak_disc.load() == 0,
         "unselected Flatpak provider must not check availability or discover");
  expect(snap_avail.load() == 0 && snap_disc.load() == 0,
         "unselected Snap provider must not check availability or discover");
  expect(gio_only_outcome.applications.size() == 1 &&
             gio_only_outcome.applications[0].provenance.provider_id == "gio",
         "GIO-only discovery must return only GIO records");
  expect(gio_only_outcome.provider_summaries.size() == 1 &&
             gio_only_outcome.provider_summaries[0].provider_id == "gio",
         "GIO-only summary must include only selected provider");

  // 2. Unified discovery (all_providers) executes all registered providers.
  DiscoveryExecutionOutcome unified_outcome;
  runner.discover_async(
      [&](DiscoveryExecutionOutcome outcome) {
        unified_outcome = std::move(outcome);
      },
      DiscoveryRequestOptions::all_providers());
  loop.run_one();
  pool.join_all();

  expect(gio_avail.load() == 2 && gio_disc.load() == 2,
         "unified discovery must execute GIO");
  expect(flatpak_avail.load() == 1 && flatpak_disc.load() == 1,
         "unified discovery must execute Flatpak");
  expect(snap_avail.load() == 1 && snap_disc.load() == 1,
         "unified discovery must execute Snap");
  expect(unified_outcome.applications.size() == 3,
         "unified discovery must collect results from all 3 providers");
}

class FakeSnapBackend final : public SnapBackend {
 public:
  FakeSnapBackend(bool available, SnapBackendQueryResult result,
                  int* list_calls = nullptr)
      : available_(available),
        result_(std::move(result)),
        list_calls_(list_calls) {}

  bool is_available() const override { return available_; }

  SnapBackendQueryResult list_enabled_snaps() override {
    if (list_calls_ != nullptr) {
      ++(*list_calls_);
    }
    return result_;
  }

 private:
  bool available_;
  SnapBackendQueryResult result_;
  int* list_calls_;
};

void test_snap_missing_backend_filtering_parallel_metadata_and_stable_id() {
  // Missing / null backend.
  SnapDiscoveryProvider null_provider(nullptr);
  expect(std::string(null_provider.id()) == "snap",
         "SnapDiscoveryProvider id() must be snap");
  expect(!null_provider.is_available(), "null Snap backend is unavailable");
  expect(!null_provider.discover().ok,
         "discover() on unavailable Snap backend must fail cleanly");

  int list_calls = 0;
  {
    ApplicationDiscoveryService service;
    service.register_provider(std::make_unique<SnapDiscoveryProvider>(
        std::make_unique<FakeSnapBackend>(false, SnapBackendQueryResult{},
                                          &list_calls)));
    const DiscoveryPassResult pass = service.discover_with_report();
    expect(list_calls == 0,
           "unavailable Snap backend must not invoke list_enabled_snaps()");
    expect(pass.provider_summaries.size() == 1 &&
               pass.provider_summaries[0].status ==
                   ProviderExecutionStatus::kUnavailable,
           "unavailable Snap provider must report kUnavailable");
  }

  // Filtering, parallel instances, missing optional metadata, and exclusions.
  SnapBackendQueryResult query;
  {
    SnapInstalledRecord app;
    app.instance_name = "blender";
    app.package_name = "blender";
    app.type = "app";
    app.status = OptionalString::of("active");
    app.title = OptionalString::of("Blender");
    app.version = OptionalString::of("4.2.0");
    app.revision = OptionalString::of("4800");
    app.channel = OptionalString::of("latest/stable");
    app.summary = OptionalString::of("3D creation suite");
    app.description = OptionalString::of("Full 3D modeling and rendering.");
    app.license = OptionalString::of("GPL-3.0");
    query.snaps.push_back(std::move(app));
  }
  {
    // Parallel instance with missing optional metadata (no title/version/etc.).
    SnapInstalledRecord parallel;
    parallel.instance_name = "blender_nightly";
    parallel.package_name = "blender_nightly";
    parallel.type = "app";
    parallel.status = OptionalString::of("active");
    query.snaps.push_back(std::move(parallel));
  }
  {
    // Disabled / inactive revision must be excluded.
    SnapInstalledRecord disabled;
    disabled.instance_name = "old-app";
    disabled.package_name = "old-app";
    disabled.type = "app";
    disabled.status = OptionalString::of("installed");
    query.snaps.push_back(std::move(disabled));
  }
  for (const char* non_app_type :
       {"base", "core", "os", "kernel", "gadget", "snapd"}) {
    SnapInstalledRecord system_pkg;
    system_pkg.instance_name = std::string("sys-") + non_app_type;
    system_pkg.package_name = system_pkg.instance_name;
    system_pkg.type = non_app_type;
    system_pkg.status = OptionalString::of("active");
    query.snaps.push_back(std::move(system_pkg));
  }

  SnapDiscoveryProvider provider(
      std::make_unique<FakeSnapBackend>(true, std::move(query)));
  const DiscoveryResult result = provider.discover();
  expect(result.ok, "Snap discovery should succeed");
  expect(result.applications.size() == 2,
         "only active app-type snaps should be included");
  if (result.applications.size() != 2) return;

  const auto& main_app = result.applications[0];
  const auto& parallel_app = result.applications[1];

  expect(main_app.application_id == "snap:blender:system",
         "standard snap application_id should match contract");
  expect(main_app.provenance.provider_id == "snap" &&
             main_app.provenance.native_id == "blender" &&
             main_app.provenance.installation_source ==
                 InstallationSource::kSnap &&
             main_app.provenance.installation_scope ==
                 InstallationScope::kSystem,
         "Snap provenance must match contract");
  expect(main_app.package_id.has_value() &&
             main_app.package_id.value() == "blender",
         "base package_id should be blender");
  expect(!main_app.installation_id.has_value(),
         "non-parallel snap should have unset installation_id");
  expect(main_app.metadata.name.has_value() &&
             main_app.metadata.name.value() == "Blender",
         "title should populate metadata.name");
  expect(main_app.metadata.version.has_value() &&
             main_app.metadata.version.value() == "4.2.0",
         "version should be populated");
  expect(main_app.metadata.revision.has_value() &&
             main_app.metadata.revision.value() == "4800",
         "revision should be populated");
  expect(main_app.metadata.channel.has_value() &&
             main_app.metadata.channel.value() == "latest/stable",
         "channel should be populated");
  expect(main_app.metadata.summary.has_value() &&
             main_app.metadata.summary.value() == "3D creation suite",
         "summary should be populated");
  expect(main_app.metadata.description.has_value() &&
             main_app.metadata.description.value() ==
                 "Full 3D modeling and rendering.",
         "description should be populated");
  expect(main_app.metadata.license.has_value() &&
             main_app.metadata.license.value() == "GPL-3.0",
         "license should be populated");
  expect(!main_app.desktop_file_id.has_value() &&
             !main_app.metadata.executable.has_value() &&
             main_app.classification == ApplicationClassification::kUnknown &&
             main_app.desktop_menu_visibility ==
                 DesktopMenuVisibility::kUnknown &&
             main_app.capabilities.launchable == CapabilityState::kUnknown,
         "Snap provider must not fabricate desktop ID, executable, "
         "classification, visibility, or capabilities");

  // Parallel install checks.
  expect(parallel_app.application_id == "snap:blender_nightly:system:nightly",
         "parallel snap must preserve distinct instance identity");
  expect(parallel_app.application_id != main_app.application_id,
         "parallel install must not collide with default install");
  expect(parallel_app.package_id.has_value() &&
             parallel_app.package_id.value() == "blender",
         "parallel install package_id should resolve to base snap name");
  expect(parallel_app.installation_id.has_value() &&
             parallel_app.installation_id.value() == "nightly",
         "parallel install should record instance key in installation_id");
  expect(parallel_app.metadata.name.has_value() &&
             parallel_app.metadata.name.value() == "blender_nightly",
         "missing title should fall back to instance name");
  expect(!parallel_app.metadata.version.has_value() &&
             !parallel_app.metadata.revision.has_value() &&
             !parallel_app.metadata.channel.has_value() &&
             !parallel_app.metadata.summary.has_value() &&
             !parallel_app.metadata.description.has_value() &&
             !parallel_app.metadata.license.has_value(),
         "missing optional Snap metadata must stay unset");

  // Identity stability across version, revision, and title updates.
  SnapBackendQueryResult updated_query;
  SnapInstalledRecord updated;
  updated.instance_name = "blender";
  updated.package_name = "blender";
  updated.type = "app";
  updated.status = OptionalString::of("active");
  updated.title = OptionalString::of("Blender Studio");
  updated.version = OptionalString::of("4.3.1");
  updated.revision = OptionalString::of("5120");
  updated_query.snaps.push_back(std::move(updated));

  SnapDiscoveryProvider updated_provider(
      std::make_unique<FakeSnapBackend>(true, std::move(updated_query)));
  const DiscoveryResult updated_result = updated_provider.discover();
  expect(updated_result.ok && updated_result.applications.size() == 1 &&
             updated_result.applications[0].application_id ==
                 main_app.application_id,
         "Snap application_id must remain stable across updates and revisions");
}

void test_snap_provider_failure_isolation() {
  DiscoveryResult gio_result;
  gio_result.applications.push_back(make_gio_app(
      "org.example.Editor.desktop", "Editor", true, false, false));

  SnapBackendQueryResult snap_err;
  snap_err.ok = false;
  snap_err.error_kind = SnapQueryErrorKind::kPermissionDenied;
  snap_err.error_message = "access denied";

  ApplicationDiscoveryService service;
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "gio", true, std::move(gio_result)));
  service.register_provider(std::make_unique<SnapDiscoveryProvider>(
      std::make_unique<FakeSnapBackend>(true, std::move(snap_err))));

  const DiscoveryPassResult pass = service.discover_with_report();
  expect(pass.applications.size() == 1 &&
             pass.applications[0].provenance.provider_id == "gio",
         "Snap failure must not break GIO discovery");
  expect(pass.provider_summaries.size() == 2 &&
             pass.provider_summaries[1].provider_id == "snap" &&
             pass.provider_summaries[1].status ==
                 ProviderExecutionStatus::kFailed &&
             pass.provider_summaries[1].error_message.find(
                 "permission denied") != std::string::npos,
         "Snap failure must be reported accurately");
}

// --- Real mock UNIX domain socket server tests for SnapdSocketBackend ---

class MockUnixSocketServer {
 public:
  explicit MockUnixSocketServer(
      std::function<void(int client_fd, const std::string& request)> handler)
      : handler_(std::move(handler)) {
    static std::atomic<int> counter{0};
    socket_path_ = "/tmp/fda-snap-" + std::to_string(getpid()) + "-" +
                   std::to_string(counter.fetch_add(1)) + ".sock";
    unlink(socket_path_.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    expect(listen_fd_ >= 0, "mock unix socket creation must succeed");
    if (listen_fd_ < 0) return;

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
    const bool bound =
        bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) == 0;
    expect(bound, "mock unix socket bind must succeed");
    const bool listening = bound && listen(listen_fd_, 4) == 0;
    expect(listening, "mock unix socket listen must succeed");
    if (!listening) {
      close(listen_fd_);
      listen_fd_ = -1;
      return;
    }

    server_thread_ = std::thread([this]() {
      const int client = accept(listen_fd_, nullptr, nullptr);
      if (client < 0) return;
      std::string request;
      char buf[1024];
      while (request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) break;
        request.append(buf, static_cast<size_t>(n));
      }
      if (handler_) {
        handler_(client, request);
      }
      close(client);
    });
  }

  ~MockUnixSocketServer() {
    if (listen_fd_ >= 0) {
      shutdown(listen_fd_, SHUT_RDWR);
      close(listen_fd_);
    }
    if (server_thread_.joinable()) {
      server_thread_.join();
    }
    if (!socket_path_.empty()) {
      unlink(socket_path_.c_str());
    }
  }

  const std::string& path() const { return socket_path_; }

 private:
  std::string socket_path_;
  int listen_fd_ = -1;
  std::function<void(int, const std::string&)> handler_;
  std::thread server_thread_;
};

void send_all(int fd, const std::string& payload) {
  size_t offset = 0;
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  while (offset < payload.size()) {
    const ssize_t n = send(fd, payload.data() + offset,
                           payload.size() - offset, flags);
    if (n <= 0) break;
    offset += static_cast<size_t>(n);
  }
}

void test_snapd_socket_backend_end_to_end() {
  // 1. Missing socket -> unavailable.
  {
    SnapdSocketConfig cfg;
    cfg.socket_path = "/tmp/fda-snap-does-not-exist-99999.sock";
    auto backend = create_snapd_socket_backend(cfg);
    expect(!backend->is_available(),
           "missing snapd socket must report is_available() == false");
    const auto res = backend->list_enabled_snaps();
    expect(!res.ok && res.error_kind == SnapQueryErrorKind::kDaemonUnavailable,
           "missing socket query must report kDaemonUnavailable");
  }

  // 2. Stale socket (nobody listening -> ECONNREFUSED).
  {
    const std::string stale_path =
        "/tmp/fda-snap-stale-" + std::to_string(getpid()) + ".sock";
    unlink(stale_path.c_str());
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, stale_path.c_str(), stale_path.size() + 1);
    const bool bound =
        fd >= 0 &&
        bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
    if (fd >= 0) close(fd);
    expect(bound, "stale unix socket setup must succeed");

    SnapdSocketConfig cfg;
    cfg.socket_path = stale_path;
    auto backend = create_snapd_socket_backend(cfg);
    expect(backend->is_available(), "stale socket file exists as a socket");
    const auto res = backend->list_enabled_snaps();
    unlink(stale_path.c_str());
    expect(!res.ok && res.error_kind == SnapQueryErrorKind::kDaemonUnavailable,
           "refused snapd socket must report kDaemonUnavailable");
  }

  // 3. Valid HTTP/1.1 chunked response with app, parallel install, UTF-8
  // escapes, and excluded non-app types.
  {
    std::string observed_request;
    const std::string json_body =
        R"({"type":"sync","status-code":200,"status":"OK","result":[)"
        R"({"name":"code","type":"app","status":"active","title":"VS Code \u2713",)"
        R"("version":"1.90.0","revision":"160","tracking-channel":"latest/stable",)"
        R"("summary":"Code editor","description":"Extensible editor","license":"MIT"},)"
        R"({"name":"code_insiders","instance-name":"code_insiders","instance-key":"insiders",)"
        R"("type":"app","status":"active","version":"1.91.0","revision":161,"channel":"latest/edge"},)"
        R"({"name":"core22","type":"base","status":"active"},)"
        R"({"name":"snapd","type":"snapd","status":"active"})"
        R"(]})";

    // Split body into two HTTP chunks to exercise chunked decoding.
    const size_t split = json_body.size() / 2;
    char hex1[32];
    char hex2[32];
    std::snprintf(hex1, sizeof(hex1), "%zx", split);
    std::snprintf(hex2, sizeof(hex2), "%zx", json_body.size() - split);
    const std::string http =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Transfer-Encoding: chunked\r\n\r\n" +
        std::string(hex1) + "\r\n" + json_body.substr(0, split) + "\r\n" +
        std::string(hex2) + "\r\n" + json_body.substr(split) + "\r\n0\r\n\r\n";

    MockUnixSocketServer server([&](int client_fd, const std::string& req) {
      observed_request = req;
      send_all(client_fd, http);
    });

    SnapdSocketConfig cfg;
    cfg.socket_path = server.path();
    SnapDiscoveryProvider provider(create_snapd_socket_backend(cfg));
    expect(provider.is_available(), "mock snapd socket must be available");
    const DiscoveryResult result = provider.discover();

    expect(observed_request.find("GET /v2/snaps?select=enabled HTTP/1.1\r\n") ==
               0,
           "backend must request GET /v2/snaps?select=enabled");
    expect(result.ok && result.applications.size() == 2,
           "chunked snapd response should yield 2 app-type snaps");
    if (result.applications.size() == 2) {
      expect(result.applications[0].application_id == "snap:code:system",
             "first snap ID must match");
      expect(result.applications[0].metadata.name.has_value() &&
                 result.applications[0].metadata.name.value() ==
                     "VS Code \xE2\x9C\x93",
             "JSON \\u2713 escape must decode to UTF-8");
      expect(result.applications[0].metadata.channel.has_value() &&
                 result.applications[0].metadata.channel.value() ==
                     "latest/stable",
             "tracking-channel must be preferred");
      expect(result.applications[1].application_id ==
                 "snap:code_insiders:system:insiders",
             "parallel snap over socket must keep distinct identity");
      expect(result.applications[1].metadata.revision.has_value() &&
                 result.applications[1].metadata.revision.value() == "161",
             "numeric revision must decode cleanly");
    }
  }

  // 4. Successful empty result.
  {
    const std::string body =
        R"({"type":"sync","status-code":200,"status":"OK","result":[]})";
    const std::string http =
        "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\n\r\n" + body;
    MockUnixSocketServer server([&](int client_fd, const std::string&) {
      send_all(client_fd, http);
    });

    SnapdSocketConfig cfg;
    cfg.socket_path = server.path();
    SnapDiscoveryProvider provider(create_snapd_socket_backend(cfg));
    const DiscoveryResult result = provider.discover();
    expect(result.ok && result.applications.empty(),
           "empty snapd result array must succeed with 0 apps");
  }

  // 5. Permission denied (HTTP 403 error envelope).
  {
    const std::string body =
        R"({"type":"error","status-code":403,"status":"Forbidden","result":{"message":"access denied"}})";
    const std::string http =
        "HTTP/1.1 403 Forbidden\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    MockUnixSocketServer server([&](int client_fd, const std::string&) {
      send_all(client_fd, http);
    });

    SnapdSocketConfig cfg;
    cfg.socket_path = server.path();
    auto backend = create_snapd_socket_backend(cfg);
    const auto res = backend->list_enabled_snaps();
    expect(!res.ok && res.error_kind == SnapQueryErrorKind::kPermissionDenied,
           "HTTP 403 from snapd must report kPermissionDenied");
  }

  // 6. Request timeout (server holds socket open until client times out).
  {
    Latch release_server(1);
    MockUnixSocketServer server([&](int, const std::string&) {
      release_server.wait();
    });

    SnapdSocketConfig cfg;
    cfg.socket_path = server.path();
    cfg.connect_timeout_ms = 200;
    cfg.request_timeout_ms = 60;
    auto backend = create_snapd_socket_backend(cfg);
    const auto res = backend->list_enabled_snaps();
    release_server.count_down();
    expect(!res.ok && res.error_kind == SnapQueryErrorKind::kTimeout,
           "hung snapd server must trigger bounded kTimeout");
  }

  // 7. Malformed HTTP response.
  {
    MockUnixSocketServer server([&](int client_fd, const std::string&) {
      send_all(client_fd, "NOT-HTTP\r\n\r\n{}");
    });

    SnapdSocketConfig cfg;
    cfg.socket_path = server.path();
    auto backend = create_snapd_socket_backend(cfg);
    const auto res = backend->list_enabled_snaps();
    expect(!res.ok && res.error_kind == SnapQueryErrorKind::kProtocolError,
           "malformed HTTP response must report kProtocolError");
  }

  // 8. Malformed JSON response.
  {
    const std::string body = "{\"type\":\"sync\",\"result\":[unterminated";
    const std::string http =
        "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\n\r\n" + body;
    MockUnixSocketServer server([&](int client_fd, const std::string&) {
      send_all(client_fd, http);
    });

    SnapdSocketConfig cfg;
    cfg.socket_path = server.path();
    auto backend = create_snapd_socket_backend(cfg);
    const auto res = backend->list_enabled_snaps();
    expect(!res.ok && res.error_kind == SnapQueryErrorKind::kProtocolError,
           "malformed JSON must report kProtocolError");
  }

  // 9. Oversized response rejected cleanly.
  {
    const std::string body(2048, 'a');
    const std::string http =
        "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\n\r\n" + body;
    MockUnixSocketServer server([&](int client_fd, const std::string&) {
      send_all(client_fd, http);
    });

    SnapdSocketConfig cfg;
    cfg.socket_path = server.path();
    cfg.max_response_bytes = 256;
    auto backend = create_snapd_socket_backend(cfg);
    const auto res = backend->list_enabled_snaps();
    expect(!res.ok && res.error_kind == SnapQueryErrorKind::kProtocolError,
           "oversized snapd response must be rejected with kProtocolError");
  }
}

}  // namespace

int main() {
  test_unified_model_defaults_are_unknown_and_missing();
  test_visibility_states_remain_independent_from_source_and_classification();
  test_unified_model_distinguishes_empty_vs_missing_and_false_vs_unknown();
  test_identity_contract_and_collision_resistance();
  test_legacy_desktop_file_id_validation();
  test_legacy_adapter_enforces_gio_visibility_and_preserves_order();
  test_coordinator_report_distinguishes_success_unavailable_and_failure();
  test_async_completion_context_empty_and_provider_failures();
  test_async_worker_exception_produces_execution_error();
  test_async_serializes_overlapping_requests_and_keeps_platform_responsive();
  test_async_cancellation_for_queued_and_in_flight_requests();
  test_teardown_while_discovery_pending_is_non_blocking_and_safe();
  test_teardown_with_stopped_platform_loop_releases_pending_calls();
  test_concurrent_registration_and_discovery_on_service();
  test_flatpak_missing_backend_or_library_is_unavailable();
  test_flatpak_empty_installations();
  test_flatpak_scopes_named_installs_branches_arches_metadata_and_runtimes();
  test_flatpak_partial_and_total_installation_failures();
  test_flatpak_ownership_and_legacy_list_apps_compatibility();
  test_provider_filtering_legacy_gio_only_vs_unified();
  test_snap_missing_backend_filtering_parallel_metadata_and_stable_id();
  test_snap_provider_failure_isolation();
  test_snapd_socket_backend_end_to_end();

  if (g_failures > 0) {
    std::cerr << g_failures << " test assertion(s) failed.\n";
    return 1;
  }
  std::cout << "All discovery native unit and async tests passed.\n";
  return 0;
}
