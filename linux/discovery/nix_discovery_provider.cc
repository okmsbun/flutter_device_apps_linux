#include "nix_discovery_provider.h"

#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "application_identity.h"
#include "json_parser.h"

namespace flutter_device_apps_linux {
namespace {

constexpr size_t kNixStoreHashLength = 32;
constexpr const char* kNixStorePrefix = "/nix/store/";

std::string normalize_logical_path(const std::string& path) {
  std::string out = path;
  while (out.size() > 1 && out.back() == '/') {
    out.pop_back();
  }
  return out;
}

std::string join_path(const std::string& base, const std::string& child) {
  if (base.empty()) return child;
  if (base.back() == '/') return base + child;
  return base + "/" + child;
}

std::string get_env_string(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

std::string resolve_username(const NixDiscoveryConfig& config) {
  if (config.user_name_override.has_value()) {
    return config.user_name_override.value();
  }
  const std::string env_user = get_env_string("USER");
  if (!env_user.empty()) return env_user;
  struct passwd* pw = getpwuid(getuid());
  if (pw != nullptr && pw->pw_name != nullptr) {
    return pw->pw_name;
  }
  return "";
}

std::string resolve_home_dir(const NixDiscoveryConfig& config) {
  if (config.home_dir_override.has_value()) {
    return config.home_dir_override.value();
  }
  const std::string env_home = get_env_string("HOME");
  if (!env_home.empty()) return env_home;
  struct passwd* pw = getpwuid(getuid());
  if (pw != nullptr && pw->pw_dir != nullptr) {
    return pw->pw_dir;
  }
  return "";
}

std::string resolve_xdg_state_home(const NixDiscoveryConfig& config,
                                   const std::string& home_dir) {
  if (config.xdg_state_home_override.has_value()) {
    if (!config.xdg_state_home_override.value().empty()) {
      return config.xdg_state_home_override.value();
    }
  } else {
    const std::string env_state = get_env_string("XDG_STATE_HOME");
    if (!env_state.empty()) return env_state;
  }
  if (home_dir.empty()) return "";
  return join_path(home_dir, ".local/state");
}

bool is_nix_base32_char(char ch) {
  // Nix base32 alphabet: 0-9, a-z excluding e, o, u, t.
  if (ch >= '0' && ch <= '9') return true;
  if (ch >= 'a' && ch <= 'z') {
    return ch != 'e' && ch != 'o' && ch != 'u' && ch != 't';
  }
  return false;
}

// Strips "/nix/store/<32-char-nixbase32-hash>-" when present, returning the
// derivation name segment (for example, "ripgrep-14.1.1" or "openssl-3.0-bin").
std::string extract_store_drv_name(const std::string& store_path) {
  std::string trimmed = normalize_logical_path(store_path);
  const size_t slash = trimmed.rfind('/');
  const std::string base =
      slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);

  if (trimmed.compare(0, std::strlen(kNixStorePrefix), kNixStorePrefix) == 0 &&
      base.size() > kNixStoreHashLength + 1 &&
      base[kNixStoreHashLength] == '-') {
    bool valid_hash = true;
    for (size_t i = 0; i < kNixStoreHashLength; ++i) {
      if (!is_nix_base32_char(base[i])) {
        valid_hash = false;
        break;
      }
    }
    if (valid_hash) {
      return base.substr(kNixStoreHashLength + 1);
    }
  }
  return base;
}

// Removes a known trailing "-<output>" suffix from a store derivation name when
// matching one of the element's explicit outputs (other than "out").
std::string strip_known_output_suffix(const std::string& drv_name,
                                      const std::vector<std::string>& outputs) {
  for (const auto& out : outputs) {
    if (out.empty() || out == "out") continue;
    const std::string suffix = "-" + out;
    if (drv_name.size() > suffix.size() &&
        drv_name.compare(drv_name.size() - suffix.size(), suffix.size(),
                         suffix) == 0) {
      return drv_name.substr(0, drv_name.size() - suffix.size());
    }
  }
  return drv_name;
}

// Implements Nix's documented DrvName split rule (src/libexpr/names.cc):
// splits at the first '-' immediately followed by an ASCII digit (0-9).
void split_nix_drv_name(const std::string& drv_name, std::string* pname_out,
                        OptionalString* version_out) {
  *pname_out = drv_name;
  *version_out = OptionalString::none();
  for (size_t i = 1; i + 1 < drv_name.size(); ++i) {
    if (drv_name[i] == '-' &&
        std::isdigit(static_cast<unsigned char>(drv_name[i + 1])) != 0) {
      *pname_out = drv_name.substr(0, i);
      *version_out = OptionalString::of(drv_name.substr(i + 1));
      return;
    }
  }
}

// Extracts architecture/system tuple (such as "x86_64-linux") from structured
// flake attrPaths like "legacyPackages.x86_64-linux.ripgrep" or
// "packages.aarch64-linux.default".
OptionalString extract_system_from_attr_path(const std::string& attr_path) {
  const char* prefixes[] = {"legacyPackages.", "packages."};
  for (const char* prefix : prefixes) {
    const size_t prefix_len = std::strlen(prefix);
    if (attr_path.compare(0, prefix_len, prefix) == 0) {
      const size_t next_dot = attr_path.find('.', prefix_len);
      if (next_dot == std::string::npos || next_dot == prefix_len) {
        return OptionalString::none();
      }
      const std::string candidate =
          attr_path.substr(prefix_len, next_dot - prefix_len);
      if (candidate.find('-') != std::string::npos) {
        return OptionalString::of(candidate);
      }
    }
  }
  return OptionalString::none();
}

std::string last_attr_segment(const std::string& attr_path) {
  if (attr_path.empty()) return "";
  const size_t dot = attr_path.rfind('.');
  if (dot == std::string::npos) return attr_path;
  return attr_path.substr(dot + 1);
}

std::string format_outputs_suffix(const std::vector<std::string>& outputs) {
  if (outputs.empty()) return "";
  std::vector<std::string> sorted = outputs;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::string suffix = "^";
  for (size_t i = 0; i < sorted.size(); ++i) {
    if (i > 0) suffix.push_back(',');
    suffix.append(sorted[i]);
  }
  return suffix;
}

bool resolve_canonical_dir(const std::string& path, std::string* canonical_out,
                           int* err_out) {
  char resolved[PATH_MAX];
  errno = 0;
  if (realpath(path.c_str(), resolved) == nullptr) {
    if (err_out != nullptr) *err_out = errno;
    return false;
  }
  *canonical_out = resolved;
  return true;
}

enum class FileReadStatus {
  kOk,
  kNotFound,
  kPermissionDenied,
  kNotRegular,
  kTooLarge,
  kIoError,
};

FileReadStatus read_bounded_file(const std::string& path, size_t max_bytes,
                                 std::string* out, std::string* err_detail) {
  out->clear();
  errno = 0;
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    const int err = errno;
    if (err_detail != nullptr) *err_detail = std::strerror(err);
    if (err == ENOENT || err == ENOTDIR) return FileReadStatus::kNotFound;
    if (err == EACCES || err == EPERM) return FileReadStatus::kPermissionDenied;
    return FileReadStatus::kIoError;
  }

  struct stat st;
  errno = 0;
  if (fstat(fd, &st) != 0) {
    const int err = errno;
    close(fd);
    if (err_detail != nullptr) *err_detail = std::strerror(err);
    return FileReadStatus::kIoError;
  }
  if (!S_ISREG(st.st_mode)) {
    close(fd);
    if (err_detail != nullptr) *err_detail = "manifest.json is not a regular file";
    return FileReadStatus::kNotRegular;
  }

  char buffer[8192];
  size_t total = 0;
  while (true) {
    const ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n < 0) {
      if (errno == EINTR) continue;
      const int err = errno;
      close(fd);
      out->clear();
      if (err_detail != nullptr) *err_detail = std::strerror(err);
      return FileReadStatus::kIoError;
    }
    if (n == 0) break;
    const size_t count = static_cast<size_t>(n);
    if (count > max_bytes || total > max_bytes - count) {
      close(fd);
      out->clear();
      if (err_detail != nullptr) {
        *err_detail = "manifest.json exceeds maximum allowed size";
      }
      return FileReadStatus::kTooLarge;
    }
    out->append(buffer, count);
    total += count;
  }

  close(fd);
  return FileReadStatus::kOk;
}

bool parse_element_record(const JsonValue& elem, int schema_version,
                          const std::string& map_element_name,
                          const NixProfileSpec& profile,
                          const std::string& profile_qualifier,
                          InstalledPackageRecord* out_record,
                          bool* is_inactive_out) {
  *is_inactive_out = false;
  if (elem.type != JsonValue::Type::kObject) return false;

  // Handle active/inactive state (defaults to active=true when omitted).
  const JsonValue* active_val = elem.find("active");
  if (active_val != nullptr) {
    if (active_val->type != JsonValue::Type::kBool) return false;
    if (!active_val->bool_value) {
      *is_inactive_out = true;
      return true;
    }
  }

  // Authoritative storePaths array (may contain multiple outputs).
  const JsonValue* store_paths_val = elem.find("storePaths");
  if (store_paths_val == nullptr ||
      store_paths_val->type != JsonValue::Type::kArray ||
      store_paths_val->array_value.empty()) {
    return false;
  }
  std::vector<std::string> store_paths;
  store_paths.reserve(store_paths_val->array_value.size());
  for (const auto& sp : store_paths_val->array_value) {
    if (sp.type != JsonValue::Type::kString || sp.string_value.empty()) {
      return false;
    }
    store_paths.push_back(sp.string_value);
  }

  // Optional outputs field: null (default outputs) or array of output names.
  std::vector<std::string> outputs;
  const JsonValue* outputs_val = elem.find("outputs");
  if (outputs_val != nullptr && outputs_val->type != JsonValue::Type::kNull) {
    if (outputs_val->type != JsonValue::Type::kArray) return false;
    outputs.reserve(outputs_val->array_value.size());
    for (const auto& out_item : outputs_val->array_value) {
      if (out_item.type != JsonValue::Type::kString ||
          out_item.string_value.empty()) {
        return false;
      }
      outputs.push_back(out_item.string_value);
    }
  }

  // Optional priority field.
  OptionalValue<int> priority;
  const JsonValue* prio_val = elem.find("priority");
  if (prio_val != nullptr && prio_val->type != JsonValue::Type::kNull) {
    int parsed_prio = 0;
    if (!json_signed_int(elem, "priority", &parsed_prio)) return false;
    priority = OptionalValue<int>::of(parsed_prio);
  }

  const OptionalString attr_path = json_optional_string(elem, "attrPath");
  const char* url_key = (schema_version == 1) ? "uri" : "url";
  const char* orig_url_key =
      (schema_version == 1) ? "originalUri" : "originalUrl";
  const OptionalString locked_url = json_optional_string(elem, url_key);
  OptionalString source_url = json_optional_string(elem, orig_url_key);
  if (!source_url.has_value() && locked_url.has_value()) {
    source_url = locked_url;
  }

  // Derive package name and version from the first store path without keeping
  // the volatile 32-char Nix store hash in the stable identity.
  const std::string raw_drv = extract_store_drv_name(store_paths.front());
  const std::string cleaned_drv = strip_known_output_suffix(raw_drv, outputs);
  std::string parsed_pname;
  OptionalString parsed_version;
  split_nix_drv_name(cleaned_drv, &parsed_pname, &parsed_version);

  std::string package_name;
  if (!map_element_name.empty()) {
    package_name = map_element_name;
  } else if (!parsed_pname.empty()) {
    package_name = parsed_pname;
  } else if (attr_path.has_value() && !attr_path.value().empty()) {
    package_name = last_attr_segment(attr_path.value());
  }
  if (package_name.empty()) return false;

  // Build deterministic element key that stays stable across upgrades (never
  // relies on store hashes or display names).
  std::string element_key;
  if (schema_version == 3 && !map_element_name.empty()) {
    element_key = map_element_name;
  } else {
    if (attr_path.has_value() && !attr_path.value().empty()) {
      element_key = attr_path.value();
    } else if (source_url.has_value() && !source_url.value().empty()) {
      element_key = source_url.value() + "#" + package_name;
    } else {
      element_key = package_name;
    }
    element_key.append(format_outputs_suffix(outputs));
  }

  InstalledPackageRecord record;
  record.provenance.provider_id = "nix";
  record.provenance.native_id = element_key;
  record.provenance.installation_source = InstallationSource::kNix;
  record.provenance.installation_scope = profile.scope;

  record.package_name = package_name;
  record.version = std::move(parsed_version);
  if (attr_path.has_value()) {
    record.architecture = extract_system_from_attr_path(attr_path.value());
  }
  record.profile_path = OptionalString::of(normalize_logical_path(profile.path));
  record.attribute_path = attr_path;
  record.source_url = source_url;
  record.locked_url = locked_url;
  record.outputs = std::move(outputs);
  record.store_paths = std::move(store_paths);
  record.install_priority = priority;
  record.installation_state = PackageInstallationState::kInstalled;
  record.selection_state = PackageSelectionState::kUnknown;
  record.classification = ApplicationClassification::kUnknown;

  record.package_identity_id = build_application_id(
      "nix", element_key, profile.scope, OptionalString::of(profile_qualifier));

  *out_record = std::move(record);
  return true;
}

}  // namespace

NixDiscoveryProvider::NixDiscoveryProvider(NixDiscoveryConfig config)
    : config_(std::move(config)) {}

const char* NixDiscoveryProvider::id() const {
  return "nix";
}

std::vector<NixProfileSpec> NixDiscoveryProvider::resolve_candidate_profiles()
    const {
  std::vector<NixProfileSpec> profiles;
  if (config_.include_standard_profiles) {
    const std::string home_dir = resolve_home_dir(config_);
    const std::string xdg_state_home =
        resolve_xdg_state_home(config_, home_dir);
    const std::string username = resolve_username(config_);

    if (!xdg_state_home.empty()) {
      profiles.push_back(
          {join_path(xdg_state_home, "nix/profile"), InstallationScope::kUser,
           "user"});
    }
    if (!home_dir.empty()) {
      profiles.push_back(
          {join_path(home_dir, ".nix-profile"), InstallationScope::kUser,
           "user"});
    }
    if (!username.empty() && !config_.system_profiles_root.empty()) {
      profiles.push_back(
          {join_path(config_.system_profiles_root,
                     "per-user/" + username + "/profile"),
           InstallationScope::kUser, "user"});
    }
    if (!config_.system_profiles_root.empty()) {
      profiles.push_back(
          {join_path(config_.system_profiles_root, "default"),
           InstallationScope::kSystem, "default"});
    }
  }

  for (const auto& spec : config_.explicit_profiles) {
    if (!spec.path.empty()) {
      profiles.push_back(spec);
    }
  }
  return profiles;
}

bool NixDiscoveryProvider::is_available() const {
  const auto candidates = resolve_candidate_profiles();
  for (const auto& spec : candidates) {
    struct stat st;
    if (stat(spec.path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      return true;
    }
  }
  return false;
}

PackageDiscoveryResult NixDiscoveryProvider::discover_packages() {
  PackageDiscoveryResult result;
  const auto candidates = resolve_candidate_profiles();

  std::unordered_set<std::string> seen_canonical_profiles;
  std::unordered_set<std::string> seen_package_ids;
  std::vector<std::string> diagnostics;
  size_t readable_profiles = 0;
  size_t failed_profiles = 0;

  for (const auto& spec : candidates) {
    const std::string logical_path = normalize_logical_path(spec.path);
    struct stat st;
    errno = 0;
    if (stat(logical_path.c_str(), &st) != 0) {
      const int err = errno;
      if (err == ENOENT || err == ENOTDIR) {
        continue;
      }
      ++failed_profiles;
      diagnostics.push_back(logical_path + ": " + std::strerror(err));
      continue;
    }
    if (!S_ISDIR(st.st_mode)) {
      ++failed_profiles;
      diagnostics.push_back(logical_path + ": not a profile directory");
      continue;
    }

    // Deduplicate symlink aliases pointing to the exact same active profile
    // directory while preserving distinct separate profiles/installations.
    std::string canonical_dir;
    int realpath_err = 0;
    if (!resolve_canonical_dir(logical_path, &canonical_dir, &realpath_err)) {
      ++failed_profiles;
      diagnostics.push_back(logical_path + ": " + std::strerror(realpath_err));
      continue;
    }
    if (!seen_canonical_profiles.insert(canonical_dir).second) {
      continue;
    }

    const std::string profile_qualifier =
        spec.profile_id.empty() ? logical_path : spec.profile_id;
    const std::string manifest_json_path =
        join_path(logical_path, "manifest.json");

    std::string raw_json;
    std::string read_err;
    const FileReadStatus read_status = read_bounded_file(
        manifest_json_path, config_.max_manifest_bytes, &raw_json, &read_err);

    if (read_status == FileReadStatus::kNotFound) {
      // Check for legacy nix-env manifest.nix without ever evaluating Nix code.
      const std::string legacy_nix_path =
          join_path(logical_path, "manifest.nix");
      struct stat legacy_st;
      if (lstat(legacy_nix_path.c_str(), &legacy_st) == 0) {
        ++failed_profiles;
        diagnostics.push_back(
            legacy_nix_path +
            ": unsupported legacy manifest.nix format (Nix expression "
            "evaluation is not performed)");
      } else {
        // Profile directory exists without any manifest file -> empty profile.
        ++readable_profiles;
      }
      continue;
    }

    if (read_status != FileReadStatus::kOk) {
      ++failed_profiles;
      diagnostics.push_back(manifest_json_path + ": " + read_err);
      continue;
    }

    JsonValue root;
    if (!parse_json(raw_json, &root) ||
        root.type != JsonValue::Type::kObject) {
      ++failed_profiles;
      diagnostics.push_back(manifest_json_path + ": malformed JSON manifest");
      continue;
    }

    int schema_version = 0;
    if (!json_non_negative_int(root, "version", &schema_version) ||
        (schema_version != 1 && schema_version != 2 && schema_version != 3)) {
      ++failed_profiles;
      const JsonValue* ver_val = root.find("version");
      const std::string ver_desc =
          ver_val != nullptr && !ver_val->string_value.empty()
              ? ver_val->string_value
              : "missing/invalid";
      diagnostics.push_back(manifest_json_path +
                            ": unsupported Nix manifest.json version (" +
                            ver_desc + ")");
      continue;
    }

    const JsonValue* elements = root.find("elements");
    if (elements == nullptr) {
      ++failed_profiles;
      diagnostics.push_back(manifest_json_path +
                            ": missing elements field in manifest.json");
      continue;
    }

    size_t malformed_elements = 0;
    if (schema_version == 1 || schema_version == 2) {
      if (elements->type != JsonValue::Type::kArray) {
        ++failed_profiles;
        diagnostics.push_back(
            manifest_json_path +
            ": expected JSON array for elements in manifest version " +
            std::to_string(schema_version));
        continue;
      }
      ++readable_profiles;
      for (const auto& elem : elements->array_value) {
        InstalledPackageRecord record;
        bool is_inactive = false;
        if (!parse_element_record(elem, schema_version, "", spec,
                                  profile_qualifier, &record, &is_inactive)) {
          ++malformed_elements;
          continue;
        }
        if (is_inactive) continue;
        if (!seen_package_ids.insert(record.package_identity_id).second) {
          ++malformed_elements;
          continue;
        }
        result.packages.push_back(std::move(record));
      }
    } else {
      // Manifest version 3 stores elements as an object keyed by element name.
      if (elements->type != JsonValue::Type::kObject) {
        ++failed_profiles;
        diagnostics.push_back(
            manifest_json_path +
            ": expected JSON object for elements in manifest version 3");
        continue;
      }
      ++readable_profiles;
      for (const auto& kv : elements->object_value) {
        InstalledPackageRecord record;
        bool is_inactive = false;
        if (!parse_element_record(kv.second, schema_version, kv.first, spec,
                                  profile_qualifier, &record, &is_inactive)) {
          ++malformed_elements;
          continue;
        }
        if (is_inactive) continue;
        if (!seen_package_ids.insert(record.package_identity_id).second) {
          ++malformed_elements;
          continue;
        }
        result.packages.push_back(std::move(record));
      }
    }

    if (malformed_elements > 0) {
      diagnostics.push_back(manifest_json_path + ": skipped " +
                            std::to_string(malformed_elements) +
                            " malformed or duplicate element(s)");
    }
  }

  for (size_t i = 0; i < diagnostics.size(); ++i) {
    if (i > 0) result.error_message.append("; ");
    result.error_message.append(diagnostics[i]);
  }

  if (readable_profiles == 0 && failed_profiles > 0) {
    result.ok = false;
    return result;
  }

  result.ok = true;
  return result;
}

}  // namespace flutter_device_apps_linux
