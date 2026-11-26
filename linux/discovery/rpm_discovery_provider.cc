#include "rpm_discovery_provider.h"

#include <unordered_set>
#include <utility>

#include "application_identity.h"

namespace flutter_device_apps_linux {
namespace {

std::string escape_part(const std::string& input) {
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

const char* error_prefix(RpmQueryErrorKind kind) {
  switch (kind) {
    case RpmQueryErrorKind::kNone:
      return "";
    case RpmQueryErrorKind::kLibraryUnavailable:
      return "librpm unavailable";
    case RpmQueryErrorKind::kUnsupportedAbi:
      return "unsupported librpm ABI";
    case RpmQueryErrorKind::kDatabaseUnavailable:
      return "RPM database unavailable";
    case RpmQueryErrorKind::kPermissionDenied:
      return "RPM database permission denied";
    case RpmQueryErrorKind::kQueryFailed:
      return "RPM database query failed";
  }
  return "RPM database query failed";
}

}  // namespace

std::string build_rpm_installation_identity_id(
    const std::string& package_name, const OptionalString& architecture,
    const OptionalString& epoch, const OptionalString& version,
    const OptionalString& release) {
  const std::string arch_part =
      architecture.has_value() ? architecture.value() : "";
  const std::string epoch_part = epoch.has_value() ? epoch.value() : "";
  const std::string version_part = version.has_value() ? version.value() : "";
  const std::string release_part = release.has_value() ? release.value() : "";

  std::string install_qualifier = escape_part(arch_part);
  install_qualifier.push_back('/');
  install_qualifier.append(escape_part(epoch_part));
  install_qualifier.push_back('/');
  install_qualifier.append(escape_part(version_part));
  install_qualifier.push_back('/');
  install_qualifier.append(escape_part(release_part));

  return build_application_id("rpm", package_name, InstallationScope::kSystem,
                              OptionalString::of(install_qualifier));
}

RpmDiscoveryProvider::RpmDiscoveryProvider(std::unique_ptr<RpmBackend> backend)
    : backend_(std::move(backend)) {}

const char* RpmDiscoveryProvider::id() const {
  return "rpm";
}

bool RpmDiscoveryProvider::is_available() const {
  return backend_ != nullptr && backend_->is_available();
}

std::string RpmDiscoveryProvider::abi_description() const {
  if (backend_ == nullptr) return "unavailable";
  return backend_->abi_description();
}

PackageDiscoveryResult RpmDiscoveryProvider::discover_packages() {
  PackageDiscoveryResult result;
  if (!is_available()) {
    result.ok = false;
    const std::string detail =
        backend_ != nullptr ? backend_->availability_error() : "";
    result.error_message =
        detail.empty() ? "librpm backend is unavailable" : detail;
    return result;
  }

  RpmBackendQueryResult query = backend_->list_installed_packages();
  if (!query.ok) {
    result.ok = false;
    const char* prefix = error_prefix(query.error_kind);
    if (query.error_message.empty()) {
      result.error_message = *prefix != '\0'
                                 ? std::string(prefix)
                                 : std::string("RPM database query failed");
    } else if (*prefix != '\0') {
      result.error_message = std::string(prefix) + ": " + query.error_message;
    } else {
      result.error_message = query.error_message;
    }
    return result;
  }

  size_t malformed_records = 0;
  std::unordered_set<std::string> seen_install_ids;
  result.packages.reserve(query.packages.size());

  for (auto& rpm : query.packages) {
    if (rpm.name.empty()) {
      ++malformed_records;
      continue;
    }

    InstalledPackageRecord record;
    record.provenance.provider_id = id();
    record.provenance.native_id = rpm.name;
    record.provenance.installation_source = InstallationSource::kRpm;
    record.provenance.installation_scope = InstallationScope::kSystem;

    record.package_name = rpm.name;
    record.architecture = std::move(rpm.architecture);
    record.epoch = std::move(rpm.epoch);
    record.version = std::move(rpm.version);
    record.release = std::move(rpm.release);
    record.summary = std::move(rpm.summary);
    record.description = std::move(rpm.description);
    record.license = std::move(rpm.license);
    record.homepage = std::move(rpm.homepage);
    record.vendor = std::move(rpm.vendor);
    record.package_group = std::move(rpm.package_group);
    record.installation_state = PackageInstallationState::kInstalled;
    record.selection_state = PackageSelectionState::kUnknown;
    record.classification = ApplicationClassification::kUnknown;

    // Stable package identity across version upgrades (name + architecture).
    record.package_identity_id = build_application_id(
        id(), record.package_name, InstallationScope::kSystem,
        record.architecture);

    // Version-specific installation identity distinguishing parallel installed
    // versions (such as multiple kernels) without collisions.
    const std::string install_id = build_rpm_installation_identity_id(
        record.package_name, record.architecture, record.epoch, record.version,
        record.release);
    record.installation_identity_id = OptionalString::of(install_id);

    // Guard against exact duplicate header entries if a corrupt DB yields them,
    // while preserving distinct versions and architectures.
    if (!seen_install_ids.insert(install_id).second) {
      ++malformed_records;
      continue;
    }

    result.packages.push_back(std::move(record));
  }

  if (malformed_records > 0) {
    result.error_message = "skipped " + std::to_string(malformed_records) +
                           " malformed or duplicate RPM header record(s)";
  }
  result.ok = true;
  return result;
}

}  // namespace flutter_device_apps_linux
