#include <atomic>
#include <condition_variable>
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
#include "discovery/legacy_list_apps_adapter.h"

namespace {

using flutter_device_apps_linux::ApplicationClassification;
using flutter_device_apps_linux::ApplicationDiscoveryProvider;
using flutter_device_apps_linux::ApplicationDiscoveryService;
using flutter_device_apps_linux::AsyncDiscoveryRunner;
using flutter_device_apps_linux::build_application_id;
using flutter_device_apps_linux::capability_from_bool;
using flutter_device_apps_linux::CapabilityState;
using flutter_device_apps_linux::DesktopMenuVisibility;
using flutter_device_apps_linux::DiscoveredApplication;
using flutter_device_apps_linux::DiscoveryExecutionOutcome;
using flutter_device_apps_linux::DiscoveryExecutionStatus;
using flutter_device_apps_linux::DiscoveryPassResult;
using flutter_device_apps_linux::DiscoveryRequestHandle;
using flutter_device_apps_linux::DiscoveryResult;
using flutter_device_apps_linux::InstallationScope;
using flutter_device_apps_linux::InstallationSource;
using flutter_device_apps_linux::is_valid_legacy_desktop_file_id;
using flutter_device_apps_linux::OptionalString;
using flutter_device_apps_linux::ProviderExecutionStatus;

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

  if (g_failures > 0) {
    std::cerr << g_failures << " test assertion(s) failed.\n";
    return 1;
  }
  std::cout << "All discovery native unit and async tests passed.\n";
  return 0;
}
