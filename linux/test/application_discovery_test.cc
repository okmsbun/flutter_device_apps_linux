#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "discovery/application_discovery_provider.h"
#include "discovery/application_discovery_service.h"

namespace {

using flutter_device_apps_linux::ApplicationDiscoveryProvider;
using flutter_device_apps_linux::ApplicationDiscoveryService;
using flutter_device_apps_linux::DiscoveredApplication;
using flutter_device_apps_linux::DiscoveryResult;

int g_failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

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

DiscoveredApplication make_app(const char* id, const char* name,
                               bool supports_files, bool supports_uris,
                               bool requires_terminal) {
  DiscoveredApplication app;
  app.desktop_file_id = id;
  if (name != nullptr) {
    app.app_name = name;
    app.has_app_name = true;
  }
  app.supports_files = supports_files;
  app.supports_uris = supports_uris;
  app.requires_terminal = requires_terminal;
  return app;
}

void test_empty_service_returns_empty_list() {
  ApplicationDiscoveryService service;
  expect(service.provider_count() == 0, "new service should have 0 providers");
  expect(service.discover_applications().empty(),
         "empty service should return no applications");
}

void test_null_provider_registration_is_ignored() {
  ApplicationDiscoveryService service;
  service.register_provider(nullptr);
  expect(service.provider_count() == 0,
         "null provider registration should be ignored");
  expect(service.discover_applications().empty(),
         "service with only ignored null provider should return empty list");
}

void test_empty_provider_result_is_handled() {
  ApplicationDiscoveryService service;
  int calls = 0;
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "gio", true, DiscoveryResult{}, &calls));

  expect(service.provider_count() == 1, "provider should be registered");
  const auto apps = service.discover_applications();
  expect(calls == 1, "available provider discover() should be called once");
  expect(apps.empty(), "empty provider result should yield empty app list");
}

void test_delegates_to_provider_and_preserves_order_and_fields() {
  DiscoveryResult result;
  result.applications.push_back(
      make_app("org.example.First.desktop", "First", true, false, false));
  result.applications.push_back(
      make_app("org.example.Unnamed.desktop", nullptr, false, true, true));

  ApplicationDiscoveryService service;
  int calls = 0;
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "gio", true, std::move(result), &calls));

  const auto apps = service.discover_applications();
  expect(calls == 1, "gio provider should be invoked once");
  expect(apps.size() == 2, "all applications from provider should be returned");
  if (apps.size() == 2) {
    expect(apps[0].desktop_file_id == "org.example.First.desktop",
           "first desktop_file_id should match");
    expect(apps[0].has_app_name && apps[0].app_name == "First",
           "first app_name should be preserved");
    expect(apps[0].supports_files && !apps[0].supports_uris &&
               !apps[0].requires_terminal,
           "first capability flags should match");

    expect(apps[1].desktop_file_id == "org.example.Unnamed.desktop",
           "second desktop_file_id should match");
    expect(!apps[1].has_app_name && apps[1].app_name.empty(),
           "missing app_name should preserve has_app_name == false");
    expect(!apps[1].supports_files && apps[1].supports_uris &&
               apps[1].requires_terminal,
           "second capability flags should match");
  }
}

void test_unavailable_provider_is_skipped() {
  DiscoveryResult skipped_result;
  skipped_result.applications.push_back(
      make_app("org.example.Skipped.desktop", "Skipped", false, false, false));

  ApplicationDiscoveryService service;
  int calls = 0;
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "unavailable", false, std::move(skipped_result), &calls));

  const auto apps = service.discover_applications();
  expect(calls == 0, "unavailable provider discover() should not be called");
  expect(apps.empty(), "unavailable provider should contribute no apps");
}

void test_multiple_providers_and_failure_isolation() {
  DiscoveryResult failing_result;
  failing_result.ok = false;
  failing_result.error_message = "provider failed";
  failing_result.applications.push_back(
      make_app("org.example.Partial.desktop", "Partial", false, false, false));

  DiscoveryResult first_ok;
  first_ok.applications.push_back(
      make_app("org.example.One.desktop", "One", true, false, false));

  DiscoveryResult second_ok;
  second_ok.applications.push_back(
      make_app("org.example.Two.desktop", "Two", false, true, false));

  ApplicationDiscoveryService service;
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "gio", true, std::move(first_ok)));
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "failing", true, std::move(failing_result)));
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "future", true, std::move(second_ok)));

  expect(service.provider_count() == 3, "all three providers should register");
  const auto apps = service.discover_applications();
  expect(apps.size() == 2,
         "failed provider output should be isolated while healthy providers succeed");
  if (apps.size() == 2) {
    expect(apps[0].desktop_file_id == "org.example.One.desktop",
           "first healthy provider app should stay first");
    expect(apps[1].desktop_file_id == "org.example.Two.desktop",
           "second healthy provider app should stay second");
  }
}

void test_service_owns_and_destroys_providers_deterministically() {
  int destroyed = 0;
  {
    ApplicationDiscoveryService service;
    service.register_provider(std::make_unique<FakeDiscoveryProvider>(
        "gio", true, DiscoveryResult{}, nullptr, &destroyed));
    service.register_provider(std::make_unique<FakeDiscoveryProvider>(
        "other", false, DiscoveryResult{}, nullptr, &destroyed));
    expect(destroyed == 0, "providers should remain alive while owned");
  }
  expect(destroyed == 2,
         "all registered providers should be destroyed with the service");
}

}  // namespace

int main() {
  test_empty_service_returns_empty_list();
  test_null_provider_registration_is_ignored();
  test_empty_provider_result_is_handled();
  test_delegates_to_provider_and_preserves_order_and_fields();
  test_unavailable_provider_is_skipped();
  test_multiple_providers_and_failure_isolation();
  test_service_owns_and_destroys_providers_deterministically();

  if (g_failures > 0) {
    std::cerr << g_failures << " test assertion(s) failed.\n";
    return 1;
  }
  std::cout << "All ApplicationDiscoveryService native unit tests passed.\n";
  return 0;
}
