#include "dpkg_discovery_provider.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "application_identity.h"

namespace flutter_device_apps_linux {
namespace {

class ScopedFd {
 public:
  explicit ScopedFd(int fd = -1) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  int get() const { return fd_; }

 private:
  int fd_;
};

std::string ascii_lower(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (unsigned char ch : input) {
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

std::string trim_horizontal_whitespace(const std::string& input) {
  size_t start = 0;
  while (start < input.size() &&
         (input[start] == ' ' || input[start] == '\t')) {
    ++start;
  }
  size_t end = input.size();
  while (end > start && (input[end - 1] == ' ' || input[end - 1] == '\t')) {
    --end;
  }
  return input.substr(start, end - start);
}

bool is_blank_line(const std::string& line) {
  for (char ch : line) {
    if (ch != ' ' && ch != '\t') return false;
  }
  return true;
}

enum class FieldKind {
  kIgnored,
  kPackage,
  kStatus,
  kArchitecture,
  kVersion,
  kDescription,
  kSection,
  kPriority,
  kHomepage,
  kMaintainer,
};

FieldKind classify_field_name(const std::string& raw_name) {
  const std::string name = ascii_lower(raw_name);
  if (name == "package") return FieldKind::kPackage;
  if (name == "status") return FieldKind::kStatus;
  if (name == "architecture") return FieldKind::kArchitecture;
  if (name == "version") return FieldKind::kVersion;
  if (name == "description") return FieldKind::kDescription;
  if (name == "section") return FieldKind::kSection;
  if (name == "priority") return FieldKind::kPriority;
  if (name == "homepage") return FieldKind::kHomepage;
  if (name == "maintainer") return FieldKind::kMaintainer;
  return FieldKind::kIgnored;
}

struct ParsedStatus {
  bool valid = false;
  bool is_installed = false;
  PackageSelectionState selection = PackageSelectionState::kUnknown;
  PackageInstallationState installation = PackageInstallationState::kUnknown;
};

ParsedStatus parse_dpkg_status(const std::string& value) {
  ParsedStatus out;
  std::vector<std::string> tokens;
  size_t pos = 0;
  while (pos < value.size()) {
    while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t')) {
      ++pos;
    }
    if (pos >= value.size()) break;
    const size_t start = pos;
    while (pos < value.size() && value[pos] != ' ' && value[pos] != '\t') {
      ++pos;
    }
    tokens.push_back(value.substr(start, pos - start));
  }
  if (tokens.size() != 3) return out;

  const std::string& want = tokens[0];
  const std::string& flag = tokens[1];
  const std::string& actual = tokens[2];

  if (want == "install") {
    out.selection = PackageSelectionState::kInstall;
  } else if (want == "hold") {
    out.selection = PackageSelectionState::kHold;
  } else if (want == "deinstall") {
    out.selection = PackageSelectionState::kDeinstall;
  } else if (want == "purge") {
    out.selection = PackageSelectionState::kPurge;
  } else if (want == "unknown") {
    out.selection = PackageSelectionState::kUnknown;
  } else {
    return out;
  }

  if (flag != "ok" && flag != "reinstreq") {
    return out;
  }

  if (actual == "installed") {
    out.installation = PackageInstallationState::kInstalled;
  } else if (actual == "triggers-awaited") {
    out.installation = PackageInstallationState::kTriggersAwaited;
  } else if (actual == "triggers-pending") {
    out.installation = PackageInstallationState::kTriggersPending;
  } else if (actual == "not-installed" || actual == "config-files" ||
             actual == "half-installed" || actual == "unpacked" ||
             actual == "half-configured") {
    out.installation = PackageInstallationState::kUnknown;
  } else {
    return out;
  }

  out.valid = true;
  // Actual installation requires flag == "ok" (not broken reinstreq) and an
  // installed state (installed, triggers-awaited, or triggers-pending),
  // regardless of whether desired selection is install, hold, or deinstall.
  out.is_installed = (flag == "ok") &&
                     (out.installation == PackageInstallationState::kInstalled ||
                      out.installation ==
                          PackageInstallationState::kTriggersAwaited ||
                      out.installation ==
                          PackageInstallationState::kTriggersPending);
  return out;
}

struct StanzaBuilder {
  bool has_content = false;
  bool malformed = false;
  size_t bytes = 0;
  FieldKind current_field = FieldKind::kIgnored;
  bool has_current_field = false;

  std::string package_name;
  std::string status_value;
  OptionalString architecture;
  OptionalString version;
  OptionalString summary;
  std::vector<std::string> description_lines;
  OptionalString section;
  OptionalString priority;
  OptionalString homepage;
  OptionalString maintainer;

  void reset() {
    has_content = false;
    malformed = false;
    bytes = 0;
    current_field = FieldKind::kIgnored;
    has_current_field = false;
    package_name.clear();
    status_value.clear();
    architecture.reset();
    version.reset();
    summary.reset();
    description_lines.clear();
    section.reset();
    priority.reset();
    homepage.reset();
    maintainer.reset();
  }

  void append_line(const std::string& line, size_t max_stanza_bytes) {
    has_content = true;
    if (malformed) return;
    if (line.size() > max_stanza_bytes || bytes > max_stanza_bytes - line.size()) {
      malformed = true;
      return;
    }
    bytes += line.size();

    if (line[0] == ' ' || line[0] == '\t') {
      if (!has_current_field) {
        malformed = true;
        return;
      }
      if (current_field == FieldKind::kDescription) {
        // Per Debian Policy 5.6.13, strip one leading space; a single "." line
        // denotes a blank paragraph separator.
        std::string content = line.substr(1);
        if (trim_horizontal_whitespace(content) == ".") {
          description_lines.emplace_back();
        } else {
          description_lines.push_back(std::move(content));
        }
      } else if (current_field != FieldKind::kIgnored) {
        const std::string extra = trim_horizontal_whitespace(line);
        if (!extra.empty()) {
          append_folded_field(current_field, extra);
        }
      }
      return;
    }

    const size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) {
      malformed = true;
      return;
    }

    const std::string field_name = line.substr(0, colon);
    for (unsigned char ch : field_name) {
      // Debian Policy 5.1: field names consist of printable US-ASCII except
      // space/colon (33..126 excluding ':').
      if (ch < 33 || ch > 126) {
        malformed = true;
        return;
      }
    }

    has_current_field = true;
    current_field = classify_field_name(field_name);
    const std::string value = trim_horizontal_whitespace(line.substr(colon + 1));

    switch (current_field) {
      case FieldKind::kIgnored:
        break;
      case FieldKind::kPackage:
        package_name = value;
        break;
      case FieldKind::kStatus:
        status_value = value;
        break;
      case FieldKind::kArchitecture:
        if (!value.empty()) architecture = OptionalString::of(value);
        break;
      case FieldKind::kVersion:
        if (!value.empty()) version = OptionalString::of(value);
        break;
      case FieldKind::kDescription:
        description_lines.clear();
        if (!value.empty()) summary = OptionalString::of(value);
        break;
      case FieldKind::kSection:
        if (!value.empty()) section = OptionalString::of(value);
        break;
      case FieldKind::kPriority:
        if (!value.empty()) priority = OptionalString::of(value);
        break;
      case FieldKind::kHomepage:
        if (!value.empty()) homepage = OptionalString::of(value);
        break;
      case FieldKind::kMaintainer:
        if (!value.empty()) maintainer = OptionalString::of(value);
        break;
    }
  }

 private:
  static void append_folded_value(OptionalString* target,
                                  const std::string& extra) {
    if (!target->has_value() || target->value().empty()) {
      target->set(extra);
    } else {
      target->value().push_back(' ');
      target->value().append(extra);
    }
  }

  void append_folded_field(FieldKind kind, const std::string& extra) {
    switch (kind) {
      case FieldKind::kMaintainer:
        append_folded_value(&maintainer, extra);
        break;
      case FieldKind::kHomepage:
        append_folded_value(&homepage, extra);
        break;
      case FieldKind::kSection:
        append_folded_value(&section, extra);
        break;
      case FieldKind::kPriority:
        append_folded_value(&priority, extra);
        break;
      case FieldKind::kPackage:
      case FieldKind::kStatus:
      case FieldKind::kArchitecture:
      case FieldKind::kVersion:
      case FieldKind::kDescription:
      case FieldKind::kIgnored:
        break;
    }
  }
};

void finalize_stanza(const StanzaBuilder& stanza, const char* provider_id,
                     std::vector<InstalledPackageRecord>* packages,
                     std::unordered_map<std::string, size_t>* seen_indices,
                     size_t* valid_stanzas, size_t* malformed_stanzas,
                     size_t* duplicate_stanzas) {
  if (!stanza.has_content) return;
  if (stanza.malformed || stanza.package_name.empty() ||
      stanza.status_value.empty()) {
    ++(*malformed_stanzas);
    return;
  }

  const ParsedStatus status = parse_dpkg_status(stanza.status_value);
  if (!status.valid) {
    ++(*malformed_stanzas);
    return;
  }

  ++(*valid_stanzas);
  if (!status.is_installed) return;

  InstalledPackageRecord record;
  record.provenance.provider_id = provider_id;
  record.provenance.native_id = stanza.package_name;
  record.provenance.installation_source = InstallationSource::kDpkg;
  record.provenance.installation_scope = InstallationScope::kSystem;
  record.package_name = stanza.package_name;
  record.architecture = stanza.architecture;
  record.version = stanza.version;
  record.summary = stanza.summary;
  if (!stanza.description_lines.empty()) {
    std::string full_description;
    for (size_t i = 0; i < stanza.description_lines.size(); ++i) {
      if (i > 0) full_description.push_back('\n');
      full_description.append(stanza.description_lines[i]);
    }
    record.description = OptionalString::of(std::move(full_description));
  }
  record.section = stanza.section;
  record.priority = stanza.priority;
  record.homepage = stanza.homepage;
  record.maintainer = stanza.maintainer;
  record.installation_state = status.installation;
  record.selection_state = status.selection;
  record.classification = ApplicationClassification::kUnknown;

  // Deterministic package-name + architecture identity, stable across upgrades.
  record.package_identity_id = build_application_id(
      provider_id, record.package_name, InstallationScope::kSystem,
      record.architecture);

  const auto existing = seen_indices->find(record.package_identity_id);
  if (existing != seen_indices->end()) {
    ++(*duplicate_stanzas);
    (*packages)[existing->second] = std::move(record);
  } else {
    seen_indices->emplace(record.package_identity_id, packages->size());
    packages->push_back(std::move(record));
  }
}

void append_diag(std::string* target, const std::string& message) {
  if (message.empty()) return;
  if (!target->empty()) target->append("; ");
  target->append(message);
}

}  // namespace

DpkgDiscoveryProvider::DpkgDiscoveryProvider(DpkgDiscoveryConfig config)
    : config_(std::move(config)) {}

const char* DpkgDiscoveryProvider::id() const {
  return "dpkg";
}

bool DpkgDiscoveryProvider::is_available() const {
  if (config_.status_file_path.empty()) return false;
  struct stat info {};
  if (stat(config_.status_file_path.c_str(), &info) != 0) {
    return false;
  }
  return S_ISREG(info.st_mode);
}

PackageDiscoveryResult DpkgDiscoveryProvider::discover_packages() {
  PackageDiscoveryResult result;
  if (config_.status_file_path.empty()) {
    result.ok = false;
    result.error_message = "dpkg status path is empty";
    return result;
  }

  ScopedFd fd(open(config_.status_file_path.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) {
    result.ok = false;
    result.error_message =
        std::string("unable to open dpkg status database: ") +
        std::strerror(errno);
    return result;
  }

  struct stat info {};
  if (fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
    result.ok = false;
    result.error_message = "dpkg status path is not a regular file";
    return result;
  }
  if (info.st_size < 0 ||
      static_cast<size_t>(info.st_size) > config_.max_file_bytes) {
    result.ok = false;
    result.error_message = "dpkg status database exceeds maximum allowed size";
    return result;
  }

  StanzaBuilder stanza;
  std::unordered_map<std::string, size_t> seen_indices;
  size_t valid_stanzas = 0;
  size_t malformed_stanzas = 0;
  size_t duplicate_stanzas = 0;
  size_t total_bytes = 0;

  std::string line_buffer;
  bool discarding_oversized_line = false;
  char chunk[16384];

  auto process_line = [&](std::string line) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (is_blank_line(line)) {
      finalize_stanza(stanza, id(), &result.packages, &seen_indices,
                      &valid_stanzas, &malformed_stanzas, &duplicate_stanzas);
      stanza.reset();
    } else {
      stanza.append_line(line, config_.max_stanza_bytes);
    }
  };

  while (true) {
    const ssize_t count = read(fd.get(), chunk, sizeof(chunk));
    if (count < 0) {
      if (errno == EINTR) continue;
      result.ok = false;
      result.packages.clear();
      result.error_message =
          std::string("failed reading dpkg status database: ") +
          std::strerror(errno);
      return result;
    }
    if (count == 0) break;

    const size_t n = static_cast<size_t>(count);
    if (n > config_.max_file_bytes ||
        total_bytes > config_.max_file_bytes - n) {
      result.ok = false;
      result.packages.clear();
      result.error_message =
          "dpkg status database exceeded maximum allowed size while reading";
      return result;
    }
    total_bytes += n;

    size_t start = 0;
    for (size_t i = 0; i < n; ++i) {
      if (chunk[i] == '\n') {
        if (discarding_oversized_line) {
          discarding_oversized_line = false;
          line_buffer.clear();
        } else {
          line_buffer.append(chunk + start, i - start);
          process_line(std::move(line_buffer));
          line_buffer.clear();
        }
        start = i + 1;
      }
    }
    if (start < n && !discarding_oversized_line) {
      line_buffer.append(chunk + start, n - start);
      if (line_buffer.size() > config_.max_stanza_bytes) {
        stanza.has_content = true;
        stanza.malformed = true;
        discarding_oversized_line = true;
        line_buffer.clear();
      }
    }
  }

  if (!discarding_oversized_line && !line_buffer.empty()) {
    process_line(std::move(line_buffer));
  }
  finalize_stanza(stanza, id(), &result.packages, &seen_indices,
                  &valid_stanzas, &malformed_stanzas, &duplicate_stanzas);

  std::string diagnostics;
  if (malformed_stanzas > 0) {
    append_diag(&diagnostics, "skipped " + std::to_string(malformed_stanzas) +
                                  " malformed dpkg stanza(s)");
  }
  if (duplicate_stanzas > 0) {
    append_diag(&diagnostics, "encountered " +
                                  std::to_string(duplicate_stanzas) +
                                  " duplicate dpkg package record(s)");
  }

  if (valid_stanzas == 0 && malformed_stanzas > 0) {
    result.ok = false;
    result.packages.clear();
    result.error_message = diagnostics;
    return result;
  }

  result.ok = true;
  result.error_message = std::move(diagnostics);
  return result;
}

}  // namespace flutter_device_apps_linux
