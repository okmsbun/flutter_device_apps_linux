#include "flatpak_discovery_provider.h"

#include <utility>

#include "application_identity.h"

namespace flutter_device_apps_linux {
namespace {

std::string escape_installation_part(const std::string& input) {
  static const char kHexDigits[] = "0123456789ABCDEF";
  std::string escaped;
  escaped.reserve(input.size());
  for (unsigned char ch : input) {
    if (ch == '%' || ch == '/' || ch < 0x20 || ch == 0x7F) {
      escaped.push_back('%');
      escaped.push_back(kHexDigits[(ch >> 4) & 0x0F]);
      escaped.push_back(kHexDigits[ch & 0x0F]);
    } else {
      escaped.push_back(static_cast<char>(ch));
    }
  }
  return escaped;
}

void append_diagnostic(std::string* target, const std::string& message) {
  if (message.empty()) return;
  if (!target->empty()) {
    target->append("; ");
  }
  target->append(message);
}

}  // namespace

std::string build_flatpak_installation_id(const std::string& installation_name,
                                          const std::string& branch,
                                          const std::string& arch) {
  std::string id = escape_installation_part(installation_name);
  id.push_back('/');
  id.append(escape_installation_part(branch));
  id.push_back('/');
  id.append(escape_installation_part(arch));
  return id;
}

FlatpakDiscoveryProvider::FlatpakDiscoveryProvider(
    std::unique_ptr<FlatpakBackend> backend)
    : backend_(std::move(backend)) {}

const char* FlatpakDiscoveryProvider::id() const {
  return "flatpak";
}

bool FlatpakDiscoveryProvider::is_available() const {
  return backend_ != nullptr && backend_->is_available();
}

DiscoveryResult FlatpakDiscoveryProvider::discover() {
  DiscoveryResult result;
  if (!is_available()) {
    result.ok = false;
    const std::string detail =
        backend_ != nullptr ? backend_->availability_error() : "";
    result.error_message =
        detail.empty() ? "libflatpak backend is unavailable" : detail;
    return result;
  }

  FlatpakBackendQueryResult query = backend_->list_installed_apps();
  std::string diagnostics;
  if (!query.ok) {
    append_diagnostic(&diagnostics, query.error_message.empty()
                                        ? "Flatpak backend query failed"
                                        : query.error_message);
  }

  size_t succeeded_installations = 0;
  size_t failed_installations = 0;

  for (const auto& installation : query.installations) {
    const std::string install_name =
        installation.installation_id.empty()
            ? (installation.is_user ? "user" : "default")
            : installation.installation_id;

    if (!installation.ok) {
      ++failed_installations;
      const std::string reason = installation.error_message.empty()
                                     ? "failed to list installed refs"
                                     : installation.error_message;
      append_diagnostic(&diagnostics, install_name + ": " + reason);
      continue;
    }

    ++succeeded_installations;
    const InstallationScope scope = installation.is_user
                                        ? InstallationScope::kUser
                                        : InstallationScope::kSystem;

    for (const auto& ref : installation.apps) {
      // Defense-in-depth exclusion of runtimes, SDKs, and runtime extensions.
      if (ref.ref_kind != kFlatpakRefKindApp) continue;
      if (ref.app_id.empty()) continue;

      DiscoveredApplication app;
      app.provenance.provider_id = id();
      app.provenance.native_id = ref.app_id;
      app.provenance.installation_source = InstallationSource::kFlatpak;
      app.provenance.installation_scope = scope;
      app.package_id = OptionalString::of(ref.app_id);

      const std::string install_identity =
          build_flatpak_installation_id(install_name, ref.branch, ref.arch);
      app.installation_id = OptionalString::of(install_identity);
      app.application_id = build_application_id(
          app.provenance.provider_id, app.provenance.native_id,
          app.provenance.installation_scope, app.installation_id);

      if (ref.name.has_value()) {
        app.metadata.name = ref.name;
      }
      if (ref.version.has_value()) {
        app.metadata.version = ref.version;
      }
      if (ref.summary.has_value()) {
        app.metadata.description = ref.summary;
      }
      if (!ref.arch.empty()) {
        app.metadata.architecture = OptionalString::of(ref.arch);
      }
      if (!ref.branch.empty()) {
        app.metadata.branch = OptionalString::of(ref.branch);
      }

      // Desktop file ID, capabilities, classification, and menu visibility
      // intentionally remain unknown/unset without fabricating desktop IDs
      // or assuming GUI classification.
      result.applications.push_back(std::move(app));
    }
  }

  if ((!query.ok || failed_installations > 0) &&
      succeeded_installations == 0) {
    result.ok = false;
    result.applications.clear();
    result.error_message =
        diagnostics.empty() ? "Flatpak discovery failed" : diagnostics;
    return result;
  }

  result.ok = true;
  result.error_message = std::move(diagnostics);
  return result;
}

}  // namespace flutter_device_apps_linux
