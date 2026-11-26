#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_SNAPD_SOCKET_BACKEND_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_SNAPD_SOCKET_BACKEND_H_

#include <cstddef>
#include <memory>
#include <string>

#include "snap_discovery_provider.h"

namespace flutter_device_apps_linux {

struct SnapdSocketConfig {
  std::string socket_path = "/run/snapd.socket";
  int connect_timeout_ms = 1000;
  int request_timeout_ms = 3000;
  size_t max_response_bytes = 8 * 1024 * 1024;
};

// Creates a read-only local UNIX domain socket backend that queries
// GET /v2/snaps?select=enabled over /run/snapd.socket (or a configured test
// socket path) with bounded timeouts, bounded response size, and strict
// HTTP/1.1 + RFC 8259 JSON validation.
std::unique_ptr<SnapBackend> create_snapd_socket_backend(
    SnapdSocketConfig config = SnapdSocketConfig());

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_SNAPD_SOCKET_BACKEND_H_
