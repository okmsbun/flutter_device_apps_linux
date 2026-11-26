// Linux-only reference integration test comparing DpkgDiscoveryProvider's
// direct /var/lib/dpkg/status parsing against dpkg-query output on a real
// Debian/Ubuntu host. dpkg-query is invoked ONLY by this test harness as a
// ground-truth oracle, never by production code.

#include <cstdio>
#include <iostream>
#include <map>
#include <string>

#include "discovery/dpkg_discovery_provider.h"
#include "discovery/package_ownership_indexers.h"

namespace {

using flutter_device_apps_linux::DpkgDiscoveryProvider;
using flutter_device_apps_linux::PackageDiscoveryResult;

bool is_installed_dpkg_query_status(const std::string& status) {
  // Match dpkg Status: <want> <flag> <actual> where flag == "ok" and actual is
  // installed, triggers-awaited, or triggers-pending.
  const size_t sp1 = status.find(' ');
  if (sp1 == std::string::npos) return false;
  const size_t sp2 = status.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) return false;
  const std::string flag = status.substr(sp1 + 1, sp2 - sp1 - 1);
  const std::string actual = status.substr(sp2 + 1);
  return flag == "ok" &&
         (actual == "installed" || actual == "triggers-awaited" ||
          actual == "triggers-pending");
}

}  // namespace

int main() {
  DpkgDiscoveryProvider provider;
  if (!provider.is_available()) {
    std::cerr << "FAIL: /var/lib/dpkg/status should be available on Ubuntu CI\n";
    return 1;
  }

  const PackageDiscoveryResult result = provider.discover_packages();
  if (!result.ok) {
    std::cerr << "FAIL: DpkgDiscoveryProvider failed: " << result.error_message
              << "\n";
    return 1;
  }

  std::map<std::string, std::string> parsed;
  for (const auto& pkg : result.packages) {
    const std::string arch =
        pkg.architecture.has_value() ? pkg.architecture.value() : "";
    const std::string ver = pkg.version.has_value() ? pkg.version.value() : "";
    parsed[pkg.package_name + ":" + arch] = ver;
  }

  FILE* pipe = popen(
      "dpkg-query -W -f='${Status}\\t${Package}\\t${Architecture}\\t${Version}\\n'",
      "r");
  if (pipe == nullptr) {
    std::cerr << "FAIL: unable to run dpkg-query reference command\n";
    return 1;
  }

  std::map<std::string, std::string> expected;
  char line_buf[4096];
  while (std::fgets(line_buf, sizeof(line_buf), pipe) != nullptr) {
    std::string line(line_buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    const size_t t1 = line.find('\t');
    if (t1 == std::string::npos) continue;
    const size_t t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos) continue;
    const size_t t3 = line.find('\t', t2 + 1);
    if (t3 == std::string::npos) continue;

    const std::string status = line.substr(0, t1);
    if (!is_installed_dpkg_query_status(status)) continue;
    const std::string name = line.substr(t1 + 1, t2 - t1 - 1);
    const std::string arch = line.substr(t2 + 1, t3 - t2 - 1);
    const std::string ver = line.substr(t3 + 1);
    expected[name + ":" + arch] = ver;
  }
  const int rc = pclose(pipe);
  if (rc != 0) {
    std::cerr << "FAIL: dpkg-query exited with code " << rc << "\n";
    return 1;
  }

  if (expected.empty()) {
    std::cerr << "FAIL: dpkg-query returned 0 installed packages\n";
    return 1;
  }

  if (parsed != expected) {
    std::cerr << "FAIL: DpkgDiscoveryProvider (" << parsed.size()
              << " pkgs) did not match dpkg-query (" << expected.size()
              << " pkgs)\n";
    return 1;
  }

  // Verify runtime DPKG file ownership indexer against real /var/lib/dpkg/info
  // using `dpkg -S /usr/bin/dpkg` as a test-only oracle.
  {
    auto indexer = flutter_device_apps_linux::create_dpkg_ownership_indexer();
    const auto own = indexer->build_index(
        {"/usr/bin/dpkg",
         "/usr/share/applications/fda-definitely-not-owned.desktop"},
        result.packages);
    if (!own.ok) {
      std::cerr << "FAIL: DPKG ownership indexer failed: " << own.error_message
                << "\n";
      return 1;
    }
    const auto it = own.owners_by_path.find("/usr/bin/dpkg");
    if (it == own.owners_by_path.end() || it->second.empty() ||
        it->second[0].package_name != "dpkg") {
      std::cerr << "FAIL: DPKG ownership indexer did not attribute "
                   "/usr/bin/dpkg to package dpkg\n";
      return 1;
    }
    if (own.owners_by_path.count(
            "/usr/share/applications/fda-definitely-not-owned.desktop") != 0) {
      std::cerr << "FAIL: unowned path must not be attributed by DPKG indexer\n";
      return 1;
    }

    FILE* own_pipe = popen("dpkg -S /usr/bin/dpkg", "r");
    std::string oracle_line;
    if (own_pipe != nullptr) {
      char own_buf[512];
      if (std::fgets(own_buf, sizeof(own_buf), own_pipe) != nullptr) {
        oracle_line = own_buf;
      }
      pclose(own_pipe);
    }
    if (oracle_line.compare(0, 5, "dpkg:") != 0) {
      std::cerr << "FAIL: unexpected dpkg -S oracle output: " << oracle_line
                << "\n";
      return 1;
    }
  }

  std::cout << "DPKG dpkg-query & ownership parity test passed ("
            << parsed.size() << " installed packages matched).\n";
  return 0;
}
