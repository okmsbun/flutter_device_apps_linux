#include "package_ownership_indexers.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "application_identity.h"

namespace flutter_device_apps_linux {
namespace {

enum class LineReadStatus { kOk, kMissing, kError };

// Streams a bounded regular file line-by-line (read-only, O_CLOEXEC).
template <typename LineFn>
LineReadStatus read_lines_bounded(const std::string& path, size_t max_bytes,
                                  LineFn&& on_line, std::string* error) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    const int err = errno;
    if (err == ENOENT || err == ENOTDIR) return LineReadStatus::kMissing;
    if (error != nullptr) *error = path + ": " + std::strerror(err);
    return LineReadStatus::kError;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    if (error != nullptr) *error = path + ": not a regular file";
    return LineReadStatus::kError;
  }

  std::string pending;
  char chunk[16384];
  size_t total = 0;
  bool ok = true;
  while (true) {
    const ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      if (error != nullptr) *error = path + ": " + std::strerror(errno);
      ok = false;
      break;
    }
    if (n == 0) break;
    const size_t count = static_cast<size_t>(n);
    if (count > max_bytes || total > max_bytes - count) {
      if (error != nullptr) *error = path + ": exceeds maximum allowed size";
      ok = false;
      break;
    }
    total += count;
    size_t start = 0;
    for (size_t i = 0; i < count; ++i) {
      if (chunk[i] == '\n') {
        pending.append(chunk + start, i - start);
        on_line(pending);
        pending.clear();
        start = i + 1;
      }
    }
    if (start < count) pending.append(chunk + start, count - start);
  }
  close(fd);
  if (ok && !pending.empty()) on_line(pending);
  return ok ? LineReadStatus::kOk : LineReadStatus::kError;
}

void append_diag(std::string* target, const std::string& message) {
  if (message.empty()) return;
  if (!target->empty()) target->append("; ");
  target->append(message);
}

std::unordered_set<std::string> normalize_targets(
    const std::vector<std::string>& targets) {
  std::unordered_set<std::string> out;
  out.reserve(targets.size());
  for (const auto& t : targets) {
    const std::string n = normalize_ownership_path(t);
    if (!n.empty() && n[0] == '/') out.insert(n);
  }
  return out;
}

// --- DPKG ---

class DpkgOwnershipIndexer final : public PackageOwnershipIndexer {
 public:
  explicit DpkgOwnershipIndexer(DpkgOwnershipConfig config)
      : config_(std::move(config)) {}

  const char* id() const override { return "dpkg"; }

  PackageOwnershipIndexResult build_index(
      const std::vector<std::string>& target_paths,
      const std::vector<InstalledPackageRecord>& packages) override {
    PackageOwnershipIndexResult result;
    const std::unordered_set<std::string> targets =
        normalize_targets(target_paths);
    if (targets.empty()) return result;

    // Diverted paths (original or diverted-to location) cannot be attributed
    // unambiguously from package file lists alone.
    std::unordered_set<std::string> diverted;
    {
      std::vector<std::string> triple;
      std::string diag;
      const LineReadStatus st = read_lines_bounded(
          config_.diversions_path, config_.max_diversions_bytes,
          [&](const std::string& line) {
            triple.push_back(line);
            if (triple.size() == 3) {
              diverted.insert(normalize_ownership_path(triple[0]));
              diverted.insert(normalize_ownership_path(triple[1]));
              triple.clear();
            }
          },
          &diag);
      if (st == LineReadStatus::kError) {
        append_diag(&result.error_message, "dpkg diversions unreadable: " + diag);
      }
    }

    size_t missing_lists = 0;
    size_t failed_lists = 0;
    for (const auto& pkg : packages) {
      if (pkg.provenance.provider_id != "dpkg") continue;

      std::vector<std::string> candidates;
      if (pkg.architecture.has_value() && !pkg.architecture.value().empty()) {
        candidates.push_back(config_.info_dir + "/" + pkg.package_name + ":" +
                             pkg.architecture.value() + ".list");
      }
      candidates.push_back(config_.info_dir + "/" + pkg.package_name + ".list");

      bool opened = false;
      for (const auto& list_path : candidates) {
        std::string diag;
        std::vector<std::string> hits;
        const LineReadStatus st = read_lines_bounded(
            list_path, config_.max_list_file_bytes,
            [&](const std::string& line) {
              const std::string n = normalize_ownership_path(line);
              if (targets.count(n) > 0) hits.push_back(n);
            },
            &diag);
        if (st == LineReadStatus::kMissing) continue;
        opened = true;
        if (st == LineReadStatus::kError) {
          ++failed_lists;
          break;
        }
        for (const auto& hit : hits) {
          PackageOwnershipEvidence ev;
          ev.package_provider_id = "dpkg";
          ev.package_identity_id = OptionalString::of(pkg.package_identity_id);
          ev.package_name = pkg.package_name;
          ev.architecture = pkg.architecture;
          ev.scope = InstallationScope::kSystem;
          if (diverted.count(hit) > 0) {
            ev.diverted_ambiguous = true;
            ev.diversion_detail = "path is named in /var/lib/dpkg/diversions";
          }
          result.owners_by_path[hit].push_back(std::move(ev));
        }
        break;
      }
      if (!opened) ++missing_lists;
    }

    if (missing_lists > 0) {
      append_diag(&result.error_message,
                  std::to_string(missing_lists) +
                      " dpkg package(s) without readable file list");
    }
    if (failed_lists > 0) {
      append_diag(&result.error_message,
                  std::to_string(failed_lists) +
                      " dpkg file list(s) failed to read");
    }
    return result;
  }

 private:
  DpkgOwnershipConfig config_;
};

// --- Nix ---

std::string nix_store_root_of(const std::string& path) {
  static const char kPrefix[] = "/nix/store/";
  const size_t prefix_len = sizeof(kPrefix) - 1;
  if (path.compare(0, prefix_len, kPrefix) != 0) return "";
  const size_t slash = path.find('/', prefix_len);
  return slash == std::string::npos ? path : path.substr(0, slash);
}

class NixOwnershipIndexer final : public PackageOwnershipIndexer {
 public:
  const char* id() const override { return "nix"; }

  PackageOwnershipIndexResult build_index(
      const std::vector<std::string>& target_paths,
      const std::vector<InstalledPackageRecord>& packages) override {
    PackageOwnershipIndexResult result;
    std::unordered_map<std::string, std::vector<size_t>> by_store_path;
    for (size_t i = 0; i < packages.size(); ++i) {
      if (packages[i].provenance.provider_id != "nix") continue;
      for (const auto& sp : packages[i].store_paths) {
        by_store_path[normalize_ownership_path(sp)].push_back(i);
      }
    }
    if (by_store_path.empty()) return result;

    for (const auto& raw : normalize_targets(target_paths)) {
      const std::string root = nix_store_root_of(raw);
      if (root.empty()) continue;
      const auto it = by_store_path.find(root);
      if (it == by_store_path.end()) continue;
      for (size_t idx : it->second) {
        const auto& pkg = packages[idx];
        PackageOwnershipEvidence ev;
        ev.package_provider_id = "nix";
        ev.package_identity_id = OptionalString::of(pkg.package_identity_id);
        ev.package_name = pkg.package_name;
        ev.architecture = pkg.architecture;
        ev.scope = pkg.provenance.installation_scope;
        ev.profile_path = pkg.profile_path;
        result.owners_by_path[raw].push_back(std::move(ev));
      }
    }
    return result;
  }
};

// --- RPM ---

class RpmOwnershipIndexer final : public PackageOwnershipIndexer {
 public:
  explicit RpmOwnershipIndexer(std::unique_ptr<RpmBackend> backend)
      : backend_(std::move(backend)) {}

  const char* id() const override { return "rpm"; }

  PackageOwnershipIndexResult build_index(
      const std::vector<std::string>& target_paths,
      const std::vector<InstalledPackageRecord>&) override {
    PackageOwnershipIndexResult result;
    if (backend_ == nullptr || !backend_->is_available()) {
      result.ok = false;
      result.error_message = backend_ != nullptr
                                 ? backend_->availability_error()
                                 : "librpm backend is unavailable";
      return result;
    }
    std::vector<std::string> targets;
    for (const auto& t : normalize_targets(target_paths)) targets.push_back(t);

    RpmFileOwnershipResult query = backend_->find_file_owners(targets);
    if (!query.ok) {
      result.ok = false;
      result.error_message = query.error_message;
      return result;
    }
    for (const auto& owner : query.owners) {
      PackageOwnershipEvidence ev;
      ev.package_provider_id = "rpm";
      ev.package_name = owner.package.name;
      ev.architecture = owner.package.architecture;
      ev.scope = InstallationScope::kSystem;
      ev.package_identity_id = OptionalString::of(build_application_id(
          "rpm", owner.package.name, InstallationScope::kSystem,
          owner.package.architecture));
      ev.installation_identity_id =
          OptionalString::of(build_rpm_installation_identity_id(
              owner.package.name, owner.package.architecture,
              owner.package.epoch, owner.package.version,
              owner.package.release));
      result.owners_by_path[normalize_ownership_path(owner.path)].push_back(
          std::move(ev));
    }
    result.error_message = query.error_message;
    return result;
  }

 private:
  std::unique_ptr<RpmBackend> backend_;
};

// --- Pacman ---

class PacmanOwnershipIndexer final : public PackageOwnershipIndexer {
 public:
  explicit PacmanOwnershipIndexer(std::unique_ptr<PacmanBackend> backend)
      : backend_(std::move(backend)) {}

  const char* id() const override { return "pacman"; }

  PackageOwnershipIndexResult build_index(
      const std::vector<std::string>& target_paths,
      const std::vector<InstalledPackageRecord>&) override {
    PackageOwnershipIndexResult result;
    if (backend_ == nullptr || !backend_->is_available()) {
      result.ok = false;
      result.error_message = backend_ != nullptr
                                 ? backend_->availability_error()
                                 : "libalpm backend is unavailable";
      return result;
    }
    std::vector<std::string> targets;
    for (const auto& t : normalize_targets(target_paths)) targets.push_back(t);

    PacmanFileOwnershipResult query = backend_->find_file_owners(targets);
    if (!query.ok) {
      result.ok = false;
      result.error_message = query.error_message;
      return result;
    }
    for (const auto& owner : query.owners) {
      PackageOwnershipEvidence ev;
      ev.package_provider_id = "pacman";
      ev.package_name = owner.package_name;
      ev.architecture = owner.architecture;
      ev.scope = InstallationScope::kSystem;
      ev.package_identity_id = OptionalString::of(build_application_id(
          "pacman", owner.package_name, InstallationScope::kSystem,
          owner.architecture));
      result.owners_by_path[normalize_ownership_path(owner.path)].push_back(
          std::move(ev));
    }
    result.error_message = query.error_message;
    return result;
  }

 private:
  std::unique_ptr<PacmanBackend> backend_;
};

}  // namespace

std::unique_ptr<PackageOwnershipIndexer> create_dpkg_ownership_indexer(
    DpkgOwnershipConfig config) {
  return std::unique_ptr<PackageOwnershipIndexer>(
      new DpkgOwnershipIndexer(std::move(config)));
}

std::unique_ptr<PackageOwnershipIndexer> create_nix_ownership_indexer() {
  return std::unique_ptr<PackageOwnershipIndexer>(new NixOwnershipIndexer());
}

std::unique_ptr<PackageOwnershipIndexer> create_rpm_ownership_indexer(
    std::unique_ptr<RpmBackend> backend) {
  return std::unique_ptr<PackageOwnershipIndexer>(
      new RpmOwnershipIndexer(std::move(backend)));
}

std::unique_ptr<PackageOwnershipIndexer> create_pacman_ownership_indexer(
    std::unique_ptr<PacmanBackend> backend) {
  return std::unique_ptr<PackageOwnershipIndexer>(
      new PacmanOwnershipIndexer(std::move(backend)));
}

}  // namespace flutter_device_apps_linux
