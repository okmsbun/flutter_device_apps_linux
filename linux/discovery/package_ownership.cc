#include "package_ownership.h"

#include <limits.h>
#include <cstdlib>
#include <utility>

namespace flutter_device_apps_linux {
namespace {

bool is_same_evidence(const PackageOwnershipEvidence& a,
                      const PackageOwnershipEvidence& b) {
  return a.package_provider_id == b.package_provider_id &&
         a.package_identity_id == b.package_identity_id &&
         a.installation_identity_id == b.installation_identity_id &&
         a.package_name == b.package_name && a.architecture == b.architecture &&
         a.scope == b.scope && a.profile_path == b.profile_path &&
         a.diverted_ambiguous == b.diverted_ambiguous &&
         a.diversion_detail == b.diversion_detail;
}

}  // namespace

std::string normalize_ownership_path(const std::string& path) {
  if (path.empty()) return "";
  std::string out;
  out.reserve(path.size());
  bool prev_slash = false;
  for (char ch : path) {
    if (ch == '/') {
      if (!prev_slash) {
        out.push_back('/');
        prev_slash = true;
      }
    } else {
      out.push_back(ch);
      prev_slash = false;
    }
  }
  while (out.size() > 1 && out.back() == '/') {
    out.pop_back();
  }
  return out;
}

std::string resolve_canonical_file_path(const std::string& path) {
  if (path.empty()) return "";
  char resolved[PATH_MAX];
  if (realpath(path.c_str(), resolved) == nullptr) {
    return "";
  }
  return normalize_ownership_path(resolved);
}

void CompositePackageOwnershipLookup::merge(
    const PackageOwnershipIndexResult& index_result) {
  for (const auto& kv : index_result.owners_by_path) {
    const std::string key = normalize_ownership_path(kv.first);
    if (key.empty()) continue;
    auto& bucket = owners_by_path_[key];
    for (const auto& ev : kv.second) {
      bool duplicate = false;
      for (const auto& existing : bucket) {
        if (is_same_evidence(existing, ev)) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        bucket.push_back(ev);
      }
    }
  }
}

std::vector<PackageOwnershipEvidence>
CompositePackageOwnershipLookup::lookup_file_owners(
    const std::string& file_path) const {
  const std::string normalized = normalize_ownership_path(file_path);
  if (normalized.empty()) return {};

  std::vector<PackageOwnershipEvidence> results;
  auto append_from = [&](const std::string& key) {
    const auto it = owners_by_path_.find(key);
    if (it == owners_by_path_.end()) return;
    for (const auto& ev : it->second) {
      bool duplicate = false;
      for (const auto& existing : results) {
        if (is_same_evidence(existing, ev)) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        results.push_back(ev);
      }
    }
  };

  append_from(normalized);
  const std::string canonical = resolve_canonical_file_path(normalized);
  if (!canonical.empty() && canonical != normalized) {
    append_from(canonical);
  }
  return results;
}

}  // namespace flutter_device_apps_linux
