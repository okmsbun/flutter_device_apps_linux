#include "appstream_metadata_provider.h"

#include <utility>

namespace flutter_device_apps_linux {
namespace {

const char* error_prefix(AppStreamQueryErrorKind kind) {
  switch (kind) {
    case AppStreamQueryErrorKind::kNone:
      return "";
    case AppStreamQueryErrorKind::kLibraryUnavailable:
      return "libappstream unavailable";
    case AppStreamQueryErrorKind::kUnsupportedAbi:
      return "unsupported libappstream ABI";
    case AppStreamQueryErrorKind::kCatalogUnavailable:
      return "AppStream catalog unavailable";
    case AppStreamQueryErrorKind::kMalformedMetadata:
      return "malformed AppStream metadata";
    case AppStreamQueryErrorKind::kLoadFailed:
      return "AppStream metadata load failed";
  }
  return "AppStream metadata load failed";
}

}  // namespace

AppStreamMetadataProvider::AppStreamMetadataProvider(
    std::unique_ptr<AppStreamBackend> backend)
    : backend_(std::move(backend)) {}

const char* AppStreamMetadataProvider::id() const {
  return "appstream";
}

bool AppStreamMetadataProvider::is_available() const {
  return backend_ != nullptr && backend_->is_available();
}

std::string AppStreamMetadataProvider::abi_description() const {
  if (backend_ == nullptr) return "unavailable";
  return backend_->abi_description();
}

AppStreamMetadataResult AppStreamMetadataProvider::load_metadata() {
  AppStreamMetadataResult result;
  if (!is_available()) {
    result.ok = false;
    result.error_message = "libappstream backend is unavailable";
    return result;
  }

  AppStreamBackendResult query = backend_->load_local_metadata();
  if (!query.ok) {
    result.ok = false;
    const char* prefix = error_prefix(query.error_kind);
    if (query.error_message.empty()) {
      result.error_message = *prefix != '\0'
                                 ? std::string(prefix)
                                 : std::string("AppStream metadata load failed");
    } else if (*prefix != '\0') {
      result.error_message = std::string(prefix) + ": " + query.error_message;
    } else {
      result.error_message = query.error_message;
    }
    return result;
  }

  result.records.reserve(query.records.size());
  for (auto& candidate : query.records) {
    if (candidate.component_id.empty()) continue;

    if (candidate.component_kind.empty()) {
      candidate.component_kind = "unknown";
    }
    candidate.provenance.provider_id = id();
    // Enforce invariant: AppStream metadata never proves installation.
    candidate.provenance.is_installation_proof = false;

    // Preserve all candidates, including multiple ambiguous records sharing
    // the same component_id, for future identity resolution.
    result.records.push_back(std::move(candidate));
  }

  result.ok = true;
  return result;
}

}  // namespace flutter_device_apps_linux
