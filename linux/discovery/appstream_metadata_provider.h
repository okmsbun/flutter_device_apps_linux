#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPSTREAM_METADATA_PROVIDER_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPSTREAM_METADATA_PROVIDER_H_

#include <memory>
#include <string>
#include <vector>

#include "appstream_metadata.h"

namespace flutter_device_apps_linux {

// Classifies errors reported by an AppStream metadata backend.
enum class AppStreamQueryErrorKind {
  kNone,
  kLibraryUnavailable,
  kUnsupportedAbi,
  kCatalogUnavailable,
  kMalformedMetadata,
  kLoadFailed,
};

// Raw result returned by an AppStreamBackend implementation.
struct AppStreamBackendResult {
  bool ok = true;
  AppStreamQueryErrorKind error_kind = AppStreamQueryErrorKind::kNone;
  std::string error_message;
  std::vector<AppStreamMetadataRecord> records;
};

// Internal backend seam decoupling AppStream metadata handling from runtime
// libappstream loading.
class AppStreamBackend {
 public:
  virtual ~AppStreamBackend() = default;

  // Returns whether libappstream with a supported ABI is available at runtime.
  virtual bool is_available() const = 0;

  // Human-readable identifier of the resolved ABI (for diagnostics/testing).
  virtual std::string abi_description() const = 0;

  // Loads local standardized AppStream metadata (read-only, no network).
  virtual AppStreamBackendResult load_local_metadata() = 0;
};

// Optional AppStream metadata and classification provider.
//
// Architectural boundary:
// - Intentionally separate from ApplicationDiscoveryProvider so AppStream
//   catalog entries and metainfo candidates are never emitted as confirmed
//   installed DiscoveredApplication records.
// - Preserves multiple ambiguous candidates for future identity resolution.
class AppStreamMetadataProvider final {
 public:
  explicit AppStreamMetadataProvider(std::unique_ptr<AppStreamBackend> backend);
  ~AppStreamMetadataProvider() = default;

  AppStreamMetadataProvider(const AppStreamMetadataProvider&) = delete;
  AppStreamMetadataProvider& operator=(const AppStreamMetadataProvider&) =
      delete;

  const char* id() const;
  bool is_available() const;
  std::string abi_description() const;

  // Loads local AppStream metadata candidates without modifying installed
  // application discovery results.
  AppStreamMetadataResult load_metadata();

 private:
  std::unique_ptr<AppStreamBackend> backend_;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPSTREAM_METADATA_PROVIDER_H_
