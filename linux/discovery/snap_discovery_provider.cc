#include "snap_discovery_provider.h"

#include <utility>

#include "application_identity.h"

namespace flutter_device_apps_linux {
namespace {

const char* error_prefix(SnapQueryErrorKind kind) {
  switch (kind) {
    case SnapQueryErrorKind::kNone:
      return "";
    case SnapQueryErrorKind::kDaemonUnavailable:
      return "snapd unavailable";
    case SnapQueryErrorKind::kPermissionDenied:
      return "snapd permission denied";
    case SnapQueryErrorKind::kTimeout:
      return "snapd request timed out";
    case SnapQueryErrorKind::kProtocolError:
      return "snapd protocol error";
    case SnapQueryErrorKind::kQueryFailed:
      return "snapd query failed";
  }
  return "snapd query failed";
}

bool is_active_status(const OptionalString& status) {
  if (!status.has_value() || status.value().empty()) {
    // Endpoint already filters with ?select=enabled; accept records that omit
    // an explicit status field while rejecting explicit non-active states.
    return true;
  }
  return status.value() == "active" || status.value() == "enabled";
}

}  // namespace

SnapDiscoveryProvider::SnapDiscoveryProvider(
    std::unique_ptr<SnapBackend> backend)
    : backend_(std::move(backend)) {}

const char* SnapDiscoveryProvider::id() const {
  return "snap";
}

bool SnapDiscoveryProvider::is_available() const {
  return backend_ != nullptr && backend_->is_available();
}

DiscoveryResult SnapDiscoveryProvider::discover() {
  DiscoveryResult result;
  if (!is_available()) {
    result.ok = false;
    result.error_message = "snapd backend is unavailable";
    return result;
  }

  SnapBackendQueryResult query = backend_->list_enabled_snaps();
  if (!query.ok) {
    result.ok = false;
    const char* prefix = error_prefix(query.error_kind);
    if (query.error_message.empty()) {
      result.error_message =
          *prefix != '\0' ? std::string(prefix) : std::string("snapd query failed");
    } else if (*prefix != '\0') {
      result.error_message = std::string(prefix) + ": " + query.error_message;
    } else {
      result.error_message = query.error_message;
    }
    return result;
  }

  for (const auto& snap : query.snaps) {
    // Include application-type snaps only; exclude base, core, kernel, gadget,
    // snapd, os, and any other non-application package types.
    if (snap.type != "app") continue;
    // Do not expose disabled installations or historical inactive revisions.
    if (!is_active_status(snap.status)) continue;

    std::string instance_name = snap.instance_name;
    if (instance_name.empty()) {
      instance_name = snap.package_name;
    }
    if (instance_name.empty()) continue;

    std::string package_name = snap.package_name;
    OptionalString instance_key = snap.instance_key;
    const size_t underscore = instance_name.find('_');
    if (underscore != std::string::npos) {
      if (package_name.empty() || package_name == instance_name) {
        package_name = instance_name.substr(0, underscore);
      }
      if (!instance_key.has_value() && underscore + 1 < instance_name.size()) {
        instance_key = OptionalString::of(instance_name.substr(underscore + 1));
      }
    } else if (package_name.empty()) {
      package_name = instance_name;
    }

    DiscoveredApplication app;
    app.provenance.provider_id = id();
    app.provenance.native_id = instance_name;
    app.provenance.installation_source = InstallationSource::kSnap;
    app.provenance.installation_scope = InstallationScope::kSystem;
    app.package_id = OptionalString::of(package_name);
    if (instance_key.has_value() && !instance_key.value().empty()) {
      app.installation_id = instance_key;
    }

    // Identity derives strictly from provider, authoritative instance identity,
    // system scope, and optional parallel instance key — never from mutable
    // revision numbers, versions, or display titles.
    app.application_id = build_application_id(
        app.provenance.provider_id, app.provenance.native_id,
        app.provenance.installation_scope, app.installation_id);

    if (snap.title.has_value() && !snap.title.value().empty()) {
      app.metadata.name = snap.title;
    } else {
      app.metadata.name = OptionalString::of(instance_name);
    }
    if (snap.version.has_value()) {
      app.metadata.version = snap.version;
    }
    if (snap.revision.has_value()) {
      app.metadata.revision = snap.revision;
    }
    if (snap.channel.has_value()) {
      app.metadata.channel = snap.channel;
    }
    if (snap.summary.has_value()) {
      app.metadata.summary = snap.summary;
    }
    if (snap.description.has_value()) {
      app.metadata.description = snap.description;
    }
    if (snap.license.has_value()) {
      app.metadata.license = snap.license;
    }

    // Do not fabricate desktop file IDs, executable paths, capabilities,
    // visibility states, or GUI classification.
    result.applications.push_back(std::move(app));
  }

  result.ok = true;
  return result;
}

}  // namespace flutter_device_apps_linux
