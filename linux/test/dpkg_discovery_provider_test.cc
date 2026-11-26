// Linux-only reference integration test comparing DpkgDiscoveryProvider's
// direct /var/lib/dpkg/status parsing against dpkg-query output on a real
// Debian/Ubuntu host. dpkg-query is invoked ONLY by this test harness as a
// ground-truth oracle, never by production code.

#include <cstdio>
#include <iostream>
#include <map>
#include <string>

#include "discovery/dpkg_discovery_provider.h"

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

  std::cout << "DPKG dpkg-query parity test passed (" << parsed.size()
            << " installed packages matched).\n";
  return 0;
}
