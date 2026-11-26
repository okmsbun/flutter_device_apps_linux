#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "discovery/application_discovery_provider.h"
#include "discovery/application_discovery_service.h"
#include "discovery/application_identity.h"
#include "discovery/discovered_application.h"
#include "discovery/legacy_list_apps_adapter.h"

namespace {

using flutter_device_apps_linux::ApplicationClassification;
using flutter_device_apps_linux::ApplicationDiscoveryProvider;
using flutter_device_apps_linux::ApplicationDiscoveryService;
using flutter_device_apps_linux::build_application_id;
using flutter_device_apps_linux::capability_from_bool;
using flutter_device_apps_linux::CapabilityState;
using flutter_device_apps_linux::DiscoveredApplication;
using flutter_device_apps_linux::DiscoveryResult;
using flutter_device_apps_linux::InstallationScope;
using flutter_device_apps_linux::InstallationSource;
using flutter_device_apps_linux::is_valid_legacy_desktop_file_id;
using flutter_device_apps_linux::OptionalString;

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

DiscoveredApplication make_gio_app(const char* desktop_id, const char* name,
                                   bool supports_files, bool supports_uris,
                                   bool requires_terminal) {
  DiscoveredApplication app;
  app.provenance.provider_id = "gio";
  app.provenance.native_id = desktop_id;
  app.provenance.installation_source = InstallationSource::kUnknown;
  app.provenance.installation_scope = InstallationScope::kUnknown;
  app.application_id = build_application_id(
      app.provenance.provider_id, app.provenance.native_id,
      app.provenance.installation_scope, app.installation_id);
  app.desktop_file_id = OptionalString::of(desktop_id);
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
  app.application_id = build_application_id(
      provider_id, native_id, scope, installation_id);
  return app;
}

// --- Unified model tests ---

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
  app.capabilities.launchable = CapabilityState::kUnknown;

  expect(!app.desktop_file_id.has_value(),
         "non-desktop application must not require or fabricate desktop_file_id");
  expect(app.metadata.name.has_value() && app.metadata.name.value().empty(),
         "empty metadata value must stay distinct from missing");
  expect(!app.metadata.version.has_value(),
         "missing metadata value must have has_value() == false");
  expect(app.metadata.name != app.metadata.version,
         "OptionalString::of(\"\") must not equal OptionalString::none()");
  expect(app.capabilities.supports_files == CapabilityState::kUnsupported,
         "known false must be kUnsupported");
  expect(app.capabilities.supports_uris == CapabilityState::kUnknown,
         "unknown capability must stay kUnknown");
  expect(app.capabilities.supports_files != app.capabilities.supports_uris,
         "kUnsupported must not equal kUnknown");
  expect(app.classification == ApplicationClassification::kUnknown,
         "absence of desktop entry must not imply CLI classification");
}

void test_unified_model_separates_provider_from_installation_source() {
  DiscoveredApplication app = make_gio_app(
      "org.example.Editor.desktop", "Editor", true, true, false);
  app.provenance.installation_source = InstallationSource::kFlatpak;
  app.package_id = OptionalString::of("org.example.Editor");

  expect(app.provenance.provider_id == "gio",
         "discovery provider should remain gio");
  expect(app.provenance.installation_source == InstallationSource::kFlatpak,
         "installation source should independently track flatpak");
  expect(app.desktop_file_id.has_value() &&
             app.desktop_file_id.value() == "org.example.Editor.desktop",
         "desktop file ID should remain distinct from package ID");
  expect(app.package_id.has_value() &&
             app.package_id.value() == "org.example.Editor",
         "package ID should remain distinct from desktop file ID");
}

// --- Identity contract tests ---

void test_identity_is_deterministic_and_independent_of_display_name() {
  DiscoveredApplication english = make_gio_app(
      "org.example.Editor.desktop", "Editor", true, false, false);
  DiscoveredApplication turkish = make_gio_app(
      "org.example.Editor.desktop", "Metin Duzenleyici", true, false, false);

  expect(english.application_id == "gio:org.example.Editor.desktop",
         "GIO application ID should use provider:native encoding");
  expect(english.application_id == turkish.application_id,
         "application_id must not depend on localized display name");
}

void test_identity_distinguishes_same_native_id_across_providers() {
  const std::string flatpak_id =
      build_application_id("flatpak", "org.example.Editor");
  const std::string appstream_id =
      build_application_id("appstream", "org.example.Editor");
  const std::string snap_id = build_application_id("snap", "example-editor");
  const std::string dpkg_id =
      build_application_id("dpkg", "example-editor:amd64");

  expect(flatpak_id != appstream_id,
         "identical native IDs from different providers must stay distinct");
  expect(flatpak_id == "flatpak:org.example.Editor",
         "flatpak ID should match expected format");
  expect(snap_id == "snap:example-editor",
         "snap ID should match expected format");
  expect(dpkg_id == "dpkg:example-editor%3Aamd64",
         "embedded colon in native ID must be percent-escaped");
}

void test_identity_distinguishes_multiple_installations() {
  const std::string system_install = build_application_id(
      "flatpak", "org.example.Editor", InstallationScope::kSystem);
  const std::string user_install = build_application_id(
      "flatpak", "org.example.Editor", InstallationScope::kUser);
  const std::string stable_branch = build_application_id(
      "flatpak", "org.example.Editor", InstallationScope::kUser,
      OptionalString::of("stable/x86_64"));
  const std::string beta_branch = build_application_id(
      "flatpak", "org.example.Editor", InstallationScope::kUser,
      OptionalString::of("beta/x86_64"));
  const std::string empty_install_id = build_application_id(
      "flatpak", "org.example.Editor", InstallationScope::kUser,
      OptionalString::of(""));

  expect(system_install != user_install,
         "system and user scopes must produce distinct identities");
  expect(stable_branch != beta_branch,
         "distinct installation_ids must produce distinct identities");
  expect(user_install != empty_install_id,
         "missing installation_id must not collide with empty installation_id");
}

void test_identity_prevents_concatenation_and_encoding_collisions() {
  const std::string pair_a = build_application_id("a:b", "c");
  const std::string pair_b = build_application_id("a", "b:c");
  expect(pair_a != pair_b,
         "colon in provider_id vs native_id must not collide");

  const std::string literal_escape = build_application_id("a%3Ab", "c");
  expect(pair_a != literal_escape,
         "literal %3A must be escaped so it cannot collide with encoded colon");

  const std::string colon_in_native = build_application_id("p", "n:user");
  const std::string scoped_native =
      build_application_id("p", "n", InstallationScope::kUser);
  expect(colon_in_native != scoped_native,
         "colon suffix in native_id must not collide with installation scope");

  const std::string colon_in_install = build_application_id(
      "p", "n", InstallationScope::kUser, OptionalString::of("a:b"));
  const std::string colon_in_native_with_install = build_application_id(
      "p", "n:user", InstallationScope::kUnknown, OptionalString::of("a:b"));
  expect(colon_in_install != colon_in_native_with_install,
         "escaped components must prevent multi-slot collisions");

  const std::string nul_a =
      build_application_id("p", std::string("a\0b", 3));
  const std::string nul_b = build_application_id("p", "a%00b");
  expect(nul_a != nul_b, "embedded NUL byte must encode without collision");
}

// --- Provider and coordinator tests ---

void test_empty_service_and_null_provider_handling() {
  ApplicationDiscoveryService service;
  expect(service.provider_count() == 0, "new service should have 0 providers");
  expect(service.discover_applications().empty(),
         "empty service should return no applications");

  service.register_provider(nullptr);
  expect(service.provider_count() == 0,
         "null provider registration should be ignored");
}

void test_empty_provider_result_and_unavailable_provider() {
  ApplicationDiscoveryService service;
  int gio_calls = 0;
  int unavailable_calls = 0;

  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "gio", true, DiscoveryResult{}, &gio_calls));

  DiscoveryResult skipped;
  skipped.applications.push_back(make_non_desktop_app(
      "snap", "skipped", InstallationSource::kSnap,
      InstallationScope::kSystem));
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "snap", false, std::move(skipped), &unavailable_calls));

  const auto apps = service.discover_applications();
  expect(gio_calls == 1, "available provider discover() should be called once");
  expect(unavailable_calls == 0,
         "unavailable provider discover() should not be called");
  expect(apps.empty(), "empty available + unavailable should yield empty list");
}

void test_coordinator_aggregates_preserves_provenance_and_isolates_failures() {
  DiscoveryResult gio_result;
  gio_result.applications.push_back(
      make_gio_app("org.example.Editor.desktop", "Editor", true, false, false));

  DiscoveryResult failing_result;
  failing_result.ok = false;
  failing_result.error_message = "backend unavailable";
  failing_result.applications.push_back(make_non_desktop_app(
      "rpm", "partial-pkg", InstallationSource::kRpm,
      InstallationScope::kSystem));

  DiscoveryResult dpkg_result;
  DiscoveredApplication cli = make_non_desktop_app(
      "dpkg", "jq:amd64", InstallationSource::kDpkg,
      InstallationScope::kSystem);
  cli.package_id = OptionalString::of("jq:amd64");
  cli.metadata.version = OptionalString::of("1.7.1");
  dpkg_result.applications.push_back(std::move(cli));

  ApplicationDiscoveryService service;
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "gio", true, std::move(gio_result)));
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "rpm", true, std::move(failing_result)));
  service.register_provider(std::make_unique<FakeDiscoveryProvider>(
      "dpkg", true, std::move(dpkg_result)));

  expect(service.provider_count() == 3, "all three providers should register");
  const auto apps = service.discover_applications();
  expect(apps.size() == 2,
         "failed provider should be isolated while healthy providers aggregate");
  if (apps.size() == 2) {
    expect(apps[0].application_id == "gio:org.example.Editor.desktop",
           "GIO identity should be preserved");
    expect(apps[0].provenance.provider_id == "gio" &&
               apps[0].provenance.native_id == "org.example.Editor.desktop",
           "GIO provenance should be preserved");

    expect(apps[1].application_id == "dpkg:jq%3Aamd64:system",
           "non-desktop provider identity should be preserved");
    expect(!apps[1].desktop_file_id.has_value(),
           "non-desktop record should remain without desktop_file_id");
    expect(apps[1].provenance.provider_id == "dpkg" &&
               apps[1].provenance.installation_source ==
                   InstallationSource::kDpkg &&
               apps[1].provenance.installation_scope ==
                   InstallationScope::kSystem,
           "non-desktop source provenance should be preserved");
  }
}

void test_service_owns_and_destroys_providers_deterministically() {
  int destroyed = 0;
  {
    ApplicationDiscoveryService service;
    service.register_provider(std::make_unique<FakeDiscoveryProvider>(
        "gio", true, DiscoveryResult{}, nullptr, &destroyed));
    service.register_provider(std::make_unique<FakeDiscoveryProvider>(
        "dpkg", false, DiscoveryResult{}, nullptr, &destroyed));
    expect(destroyed == 0, "providers should remain alive while owned");
  }
  expect(destroyed == 2,
         "all registered providers should be destroyed with the service");
}

// --- Legacy serialization adapter tests ---

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

void test_legacy_adapter_preserves_contract_and_filters_incompatible_records() {
  std::vector<DiscoveredApplication> unified;

  // 1. Standard GIO visible entry.
  unified.push_back(make_gio_app(
      "org.example.First.desktop", "First", true, false, true));

  // 2. Non-desktop application (must be filtered out of legacy listApps).
  DiscoveredApplication non_desktop = make_non_desktop_app(
      "dpkg", "curl:amd64", InstallationSource::kDpkg,
      InstallationScope::kSystem);
  non_desktop.metadata.name = OptionalString::of("curl");
  unified.push_back(std::move(non_desktop));

  // 3. Invalid desktop ID (must be filtered out, never fabricated).
  DiscoveredApplication bad_desktop = make_non_desktop_app(
      "flatpak", "org.example.Broken", InstallationSource::kFlatpak,
      InstallationScope::kUser);
  bad_desktop.desktop_file_id = OptionalString::of("/usr/share/bad.desktop");
  unified.push_back(std::move(bad_desktop));

  // 4. Desktop entry with missing app_name (null in legacy map) and unknown
  // capability states (mapped to false in legacy bool schema).
  DiscoveredApplication unnamed = make_gio_app(
      "org.example.Unnamed.desktop", nullptr, false, true, false);
  unnamed.capabilities.supports_files = CapabilityState::kUnknown;
  unnamed.capabilities.launchable = CapabilityState::kSupported;
  unnamed.metadata.version = OptionalString::of("2.0.0");
  unified.push_back(std::move(unnamed));

  // 5. Desktop entry with explicitly empty app_name (present "" in legacy map).
  unified.push_back(
      make_gio_app("org.example.EmptyName.desktop", "", false, false, false));

  const auto legacy =
      flutter_device_apps_linux::adapt_for_legacy_list_apps(unified);
  expect(legacy.size() == 3,
         "legacy adapter should keep only valid desktop-integrated records");
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
           "missing app_name must stay unset so legacy serializer omits key");
    expect(!legacy[1].supports_files && legacy[1].supports_uris &&
               !legacy[1].requires_terminal,
           "unknown capability should map to false while known true stays true");

    expect(legacy[2].desktop_file_id == "org.example.EmptyName.desktop",
           "third legacy record order should be preserved");
    expect(legacy[2].app_name.has_value() && legacy[2].app_name.value().empty(),
           "empty app_name must stay present as empty string");
  }
}

}  // namespace

int main() {
  test_unified_model_defaults_are_unknown_and_missing();
  test_unified_model_distinguishes_empty_vs_missing_and_false_vs_unknown();
  test_unified_model_separates_provider_from_installation_source();
  test_identity_is_deterministic_and_independent_of_display_name();
  test_identity_distinguishes_same_native_id_across_providers();
  test_identity_distinguishes_multiple_installations();
  test_identity_prevents_concatenation_and_encoding_collisions();
  test_empty_service_and_null_provider_handling();
  test_empty_provider_result_and_unavailable_provider();
  test_coordinator_aggregates_preserves_provenance_and_isolates_failures();
  test_service_owns_and_destroys_providers_deterministically();
  test_legacy_desktop_file_id_validation();
  test_legacy_adapter_preserves_contract_and_filters_incompatible_records();

  if (g_failures > 0) {
    std::cerr << g_failures << " test assertion(s) failed.\n";
    return 1;
  }
  std::cout << "All discovery native unit tests passed.\n";
  return 0;
}
