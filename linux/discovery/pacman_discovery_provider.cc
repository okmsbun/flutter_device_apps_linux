#include "pacman_discovery_provider.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <unordered_set>
#include <utility>

#include "application_identity.h"

namespace flutter_device_apps_linux {
namespace {

constexpr size_t kMaxPacmanConfBytes = 1024 * 1024;  // 1 MiB safety bound.
constexpr const char* kDefaultRootDir = "/";
constexpr const char* kDefaultDbRelativePath = "var/lib/pacman/";

std::string trim_ascii(const std::string& value) {
  size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string join_root_and_relative(const std::string& root,
                                   const std::string& relative) {
  if (root.empty()) return "/" + relative;
  if (root.back() == '/') return root + relative;
  return root + "/" + relative;
}

bool read_bounded_regular_file(const std::string& path, std::string* out) {
  if (path.empty() || out == nullptr) return false;
  out->clear();

  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;

  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return false;
  }

  char buffer[4096];
  size_t total = 0;
  while (total < kMaxPacmanConfBytes) {
    const size_t remaining = kMaxPacmanConfBytes - total;
    const size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const ssize_t n = read(fd, buffer, chunk);
    if (n < 0) {
      if (errno == EINTR) continue;
      close(fd);
      out->clear();
      return false;
    }
    if (n == 0) break;
    out->append(buffer, static_cast<size_t>(n));
    total += static_cast<size_t>(n);
  }

  close(fd);
  return true;
}

const char* error_prefix(PacmanQueryErrorKind kind) {
  switch (kind) {
    case PacmanQueryErrorKind::kNone:
      return "";
    case PacmanQueryErrorKind::kLibraryUnavailable:
      return "libalpm unavailable";
    case PacmanQueryErrorKind::kUnsupportedAbi:
      return "unsupported libalpm ABI";
    case PacmanQueryErrorKind::kDatabaseUnavailable:
      return "Pacman database unavailable";
    case PacmanQueryErrorKind::kPermissionDenied:
      return "Pacman database permission denied";
    case PacmanQueryErrorKind::kQueryFailed:
      return "Pacman database query failed";
  }
  return "Pacman database query failed";
}

}  // namespace

PacmanResolvedPaths resolve_pacman_db_paths(
    const std::string& pacman_conf_path, const std::string& root_dir_override,
    const std::string& db_path_override) {
  std::string conf_root;
  std::string conf_db_path;

  std::string content;
  if (read_bounded_regular_file(pacman_conf_path, &content)) {
    bool in_options_section = false;
    size_t pos = 0;
    while (pos < content.size()) {
      size_t line_end = content.find('\n', pos);
      if (line_end == std::string::npos) line_end = content.size();
      std::string line = content.substr(pos, line_end - pos);
      pos = line_end + 1;

      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      const std::string trimmed = trim_ascii(line);
      if (trimmed.empty() || trimmed[0] == '#') {
        continue;
      }

      if (trimmed.front() == '[' && trimmed.back() == ']' &&
          trimmed.size() >= 2) {
        const std::string section =
            trim_ascii(trimmed.substr(1, trimmed.size() - 2));
        in_options_section = (section == "options");
        continue;
      }

      if (!in_options_section) continue;

      const size_t eq = trimmed.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = trim_ascii(trimmed.substr(0, eq));
      const std::string value = trim_ascii(trimmed.substr(eq + 1));
      if (value.empty()) continue;

      if (key == "RootDir") {
        conf_root = value;
      } else if (key == "DBPath") {
        conf_db_path = value;
      }
    }
  }

  PacmanResolvedPaths resolved;
  if (!root_dir_override.empty()) {
    resolved.root_dir = root_dir_override;
  } else if (!conf_root.empty()) {
    resolved.root_dir = conf_root;
  } else {
    resolved.root_dir = kDefaultRootDir;
  }

  if (!db_path_override.empty()) {
    resolved.db_path = db_path_override;
  } else if (!conf_db_path.empty()) {
    resolved.db_path = conf_db_path;
  } else {
    resolved.db_path =
        join_root_and_relative(resolved.root_dir, kDefaultDbRelativePath);
  }

  return resolved;
}

PacmanDiscoveryProvider::PacmanDiscoveryProvider(
    std::unique_ptr<PacmanBackend> backend)
    : backend_(std::move(backend)) {}

const char* PacmanDiscoveryProvider::id() const {
  return "pacman";
}

bool PacmanDiscoveryProvider::is_available() const {
  return backend_ != nullptr && backend_->is_available();
}

std::string PacmanDiscoveryProvider::abi_description() const {
  if (backend_ == nullptr) return "unavailable";
  return backend_->abi_description();
}

PackageDiscoveryResult PacmanDiscoveryProvider::discover_packages() {
  PackageDiscoveryResult result;
  if (!is_available()) {
    result.ok = false;
    const std::string detail =
        backend_ != nullptr ? backend_->availability_error() : "";
    result.error_message =
        detail.empty() ? "libalpm backend is unavailable" : detail;
    return result;
  }

  PacmanBackendQueryResult query = backend_->list_installed_packages();
  if (!query.ok) {
    result.ok = false;
    const char* prefix = error_prefix(query.error_kind);
    if (query.error_message.empty()) {
      result.error_message = *prefix != '\0'
                                 ? std::string(prefix)
                                 : std::string("Pacman database query failed");
    } else if (*prefix != '\0') {
      result.error_message = std::string(prefix) + ": " + query.error_message;
    } else {
      result.error_message = query.error_message;
    }
    return result;
  }

  size_t malformed_records = 0;
  std::unordered_set<std::string> seen_package_ids;
  result.packages.reserve(query.packages.size());

  for (auto& pkg : query.packages) {
    if (pkg.name.empty()) {
      ++malformed_records;
      continue;
    }

    InstalledPackageRecord record;
    record.provenance.provider_id = id();
    record.provenance.native_id = pkg.name;
    record.provenance.installation_source = InstallationSource::kPacman;
    record.provenance.installation_scope = InstallationScope::kSystem;

    record.package_name = pkg.name;
    record.version = std::move(pkg.version);
    record.architecture = std::move(pkg.architecture);
    record.description = std::move(pkg.description);
    record.homepage = std::move(pkg.homepage);
    for (auto& lic : pkg.licenses) {
      if (!lic.empty()) {
        record.licenses.push_back(std::move(lic));
      }
    }
    for (auto& grp : pkg.groups) {
      if (!grp.empty()) {
        record.groups.push_back(std::move(grp));
      }
    }
    record.packager = std::move(pkg.packager);
    record.installation_state = PackageInstallationState::kInstalled;
    record.selection_state = PackageSelectionState::kUnknown;
    record.install_reason = pkg.install_reason;
    record.classification = ApplicationClassification::kUnknown;

    // Deterministic package identity stable across version upgrades.
    record.package_identity_id = build_application_id(
        id(), record.package_name, InstallationScope::kSystem,
        record.architecture);

    if (!seen_package_ids.insert(record.package_identity_id).second) {
      ++malformed_records;
      continue;
    }

    result.packages.push_back(std::move(record));
  }

  if (malformed_records > 0) {
    result.error_message = "skipped " + std::to_string(malformed_records) +
                           " malformed or duplicate Pacman package record(s)";
  }
  result.ok = true;
  return result;
}

}  // namespace flutter_device_apps_linux
