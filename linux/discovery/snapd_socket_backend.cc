#include "snapd_socket_backend.h"

#include "json_parser.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace flutter_device_apps_linux {
namespace {

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

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

int remaining_ms(TimePoint deadline) {
  const auto now = SteadyClock::now();
  if (now >= deadline) return 0;
  const auto diff =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
          .count();
  if (diff <= 0) return 0;
  if (diff > std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(diff);
}

std::string ascii_lower(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (unsigned char ch : input) {
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

std::string trim_ascii_whitespace(const std::string& input) {
  size_t start = 0;
  while (start < input.size() &&
         (input[start] == ' ' || input[start] == '\t' ||
          input[start] == '\r' || input[start] == '\n')) {
    ++start;
  }
  size_t end = input.size();
  while (end > start &&
         (input[end - 1] == ' ' || input[end - 1] == '\t' ||
          input[end - 1] == '\r' || input[end - 1] == '\n')) {
    --end;
  }
  return input.substr(start, end - start);
}

SnapQueryErrorKind classify_connect_errno(int err) {
  switch (err) {
    case EACCES:
    case EPERM:
      return SnapQueryErrorKind::kPermissionDenied;
    case ENOENT:
    case ENOTDIR:
    case ECONNREFUSED:
    case ENOTSOCK:
      return SnapQueryErrorKind::kDaemonUnavailable;
    case ETIMEDOUT:
      return SnapQueryErrorKind::kTimeout;
    default:
      return SnapQueryErrorKind::kQueryFailed;
  }
}

// --- Strict bounded HTTP/1.1 response decoder ---

struct HttpResponse {
  int status_code = 0;
  std::string body;
};

bool parse_strict_size(const std::string& text, size_t* out) {
  if (text.empty()) return false;
  size_t value = 0;
  for (unsigned char ch : text) {
    if (ch < '0' || ch > '9') return false;
    const size_t digit = static_cast<size_t>(ch - '0');
    if (value > (std::numeric_limits<size_t>::max() - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

bool decode_chunked_body(const std::string& raw, size_t max_bytes,
                         std::string* decoded) {
  decoded->clear();
  size_t pos = 0;
  while (true) {
    const size_t line_end = raw.find("\r\n", pos);
    if (line_end == std::string::npos) return false;

    std::string size_str = raw.substr(pos, line_end - pos);
    const size_t semicolon = size_str.find(';');
    if (semicolon != std::string::npos) {
      size_str = size_str.substr(0, semicolon);
    }
    size_str = trim_ascii_whitespace(size_str);
    if (size_str.empty()) return false;

    size_t chunk_size = 0;
    for (unsigned char ch : size_str) {
      size_t digit = 0;
      if (ch >= '0' && ch <= '9') {
        digit = static_cast<size_t>(ch - '0');
      } else if (ch >= 'a' && ch <= 'f') {
        digit = static_cast<size_t>(10 + (ch - 'a'));
      } else if (ch >= 'A' && ch <= 'F') {
        digit = static_cast<size_t>(10 + (ch - 'A'));
      } else {
        return false;
      }
      if (chunk_size > (std::numeric_limits<size_t>::max() - digit) / 16) {
        return false;
      }
      chunk_size = chunk_size * 16 + digit;
    }

    pos = line_end + 2;
    if (chunk_size == 0) {
      // Must terminate with empty trailer line (\r\n) or trailer headers ending
      // in \r\n\r\n.
      if (pos + 2 <= raw.size() && raw[pos] == '\r' && raw[pos + 1] == '\n') {
        return true;
      }
      const size_t trailer_end = raw.find("\r\n\r\n", pos);
      return trailer_end != std::string::npos;
    }

    if (chunk_size > max_bytes || decoded->size() > max_bytes - chunk_size) {
      return false;
    }
    if (chunk_size > raw.size() - pos || raw.size() - pos - chunk_size < 2) {
      return false;
    }
    decoded->append(raw, pos, chunk_size);
    pos += chunk_size;
    if (raw[pos] != '\r' || raw[pos + 1] != '\n') return false;
    pos += 2;
  }
}

bool parse_http_response(const std::string& raw, size_t max_bytes,
                         HttpResponse* out, std::string* error) {
  const size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    *error = "Incomplete HTTP response headers";
    return false;
  }

  const size_t status_end = raw.find("\r\n");
  if (status_end == std::string::npos || status_end > header_end) {
    *error = "Missing HTTP status line";
    return false;
  }

  const std::string status_line = raw.substr(0, status_end);
  if (status_line.size() < 12 ||
      (status_line.compare(0, 9, "HTTP/1.1 ") != 0 &&
       status_line.compare(0, 9, "HTTP/1.0 ") != 0)) {
    *error = "Malformed HTTP status line";
    return false;
  }
  if (!std::isdigit(static_cast<unsigned char>(status_line[9])) ||
      !std::isdigit(static_cast<unsigned char>(status_line[10])) ||
      !std::isdigit(static_cast<unsigned char>(status_line[11])) ||
      (status_line.size() > 12 && status_line[12] != ' ')) {
    *error = "Malformed HTTP status code";
    return false;
  }
  out->status_code = (status_line[9] - '0') * 100 +
                     (status_line[10] - '0') * 10 + (status_line[11] - '0');

  bool has_content_length = false;
  size_t content_length = 0;
  bool is_chunked = false;

  size_t pos = status_end + 2;
  while (pos < header_end) {
    size_t line_end = raw.find("\r\n", pos);
    if (line_end == std::string::npos || line_end > header_end) {
      line_end = header_end;
    }
    const std::string line = raw.substr(pos, line_end - pos);
    const size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) {
      *error = "Malformed HTTP header line";
      return false;
    }
    const std::string name = ascii_lower(trim_ascii_whitespace(line.substr(0, colon)));
    const std::string value = trim_ascii_whitespace(line.substr(colon + 1));
    if (name == "content-length") {
      size_t parsed = 0;
      if (!parse_strict_size(value, &parsed)) {
        *error = "Invalid HTTP Content-Length header";
        return false;
      }
      if (has_content_length && parsed != content_length) {
        *error = "Conflicting HTTP Content-Length headers";
        return false;
      }
      has_content_length = true;
      content_length = parsed;
    } else if (name == "transfer-encoding") {
      const std::string lowered = ascii_lower(value);
      if (lowered.find("chunked") != std::string::npos) {
        is_chunked = true;
      }
    }
    pos = line_end + 2;
  }

  if (is_chunked && has_content_length) {
    *error = "Conflicting Transfer-Encoding and Content-Length headers";
    return false;
  }

  const std::string raw_body = raw.substr(header_end + 4);
  if (is_chunked) {
    if (!decode_chunked_body(raw_body, max_bytes, &out->body)) {
      *error = "Malformed HTTP chunked response body";
      return false;
    }
    return true;
  }

  if (has_content_length) {
    if (content_length > max_bytes) {
      *error = "HTTP response exceeds maximum allowed size";
      return false;
    }
    if (raw_body.size() < content_length) {
      *error = "Truncated HTTP response body";
      return false;
    }
    out->body = raw_body.substr(0, content_length);
    return true;
  }

  if (raw_body.size() > max_bytes) {
    *error = "HTTP response exceeds maximum allowed size";
    return false;
  }
  out->body = raw_body;
  return true;
}

// --- Snapd UNIX socket backend ---

class SnapdSocketBackend final : public SnapBackend {
 public:
  explicit SnapdSocketBackend(SnapdSocketConfig config)
      : config_(std::move(config)) {}

  bool is_available() const override {
    if (config_.socket_path.empty()) return false;
    struct stat info {};
    if (stat(config_.socket_path.c_str(), &info) != 0) {
      return false;
    }
    return S_ISSOCK(info.st_mode);
  }

  SnapBackendQueryResult list_enabled_snaps() override {
    SnapBackendQueryResult result;
    if (config_.socket_path.empty()) {
      result.ok = false;
      result.error_kind = SnapQueryErrorKind::kDaemonUnavailable;
      result.error_message = "Empty snapd socket path";
      return result;
    }

    struct sockaddr_un addr {};
    if (config_.socket_path.size() >= sizeof(addr.sun_path)) {
      result.ok = false;
      result.error_kind = SnapQueryErrorKind::kQueryFailed;
      result.error_message = "Snapd socket path exceeds UNIX socket limit";
      return result;
    }

    ScopedFd fd(socket(AF_UNIX, SOCK_STREAM, 0));
    if (fd.get() < 0) {
      result.ok = false;
      result.error_kind = SnapQueryErrorKind::kQueryFailed;
      result.error_message = std::strerror(errno);
      return result;
    }

    fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int nosigpipe = 1;
    setsockopt(fd.get(), SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe,
               sizeof(nosigpipe));
#endif
    const int flags = fcntl(fd.get(), F_GETFL, 0);
    if (flags < 0 || fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
      result.ok = false;
      result.error_kind = SnapQueryErrorKind::kQueryFailed;
      result.error_message = std::strerror(errno);
      return result;
    }

    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, config_.socket_path.c_str(),
                config_.socket_path.size() + 1);

    const TimePoint connect_deadline =
        SteadyClock::now() +
        std::chrono::milliseconds(std::max(1, config_.connect_timeout_ms));
    const TimePoint request_deadline =
        SteadyClock::now() +
        std::chrono::milliseconds(std::max(1, config_.request_timeout_ms));

    if (connect(fd.get(), reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) != 0) {
      if (errno != EINPROGRESS) {
        const int err = errno;
        result.ok = false;
        result.error_kind = classify_connect_errno(err);
        result.error_message = std::strerror(err);
        return result;
      }

      while (true) {
        const int wait_ms = remaining_ms(connect_deadline);
        if (wait_ms <= 0) {
          result.ok = false;
          result.error_kind = SnapQueryErrorKind::kTimeout;
          result.error_message = "Timed out connecting to snapd socket";
          return result;
        }
        struct pollfd pfd {};
        pfd.fd = fd.get();
        pfd.events = POLLOUT;
        const int ready = poll(&pfd, 1, wait_ms);
        if (ready < 0) {
          if (errno == EINTR) continue;
          result.ok = false;
          result.error_kind = SnapQueryErrorKind::kQueryFailed;
          result.error_message = std::strerror(errno);
          return result;
        }
        if (ready == 0) {
          result.ok = false;
          result.error_kind = SnapQueryErrorKind::kTimeout;
          result.error_message = "Timed out connecting to snapd socket";
          return result;
        }
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        if (getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &so_error, &so_len) <
            0) {
          result.ok = false;
          result.error_kind = SnapQueryErrorKind::kQueryFailed;
          result.error_message = std::strerror(errno);
          return result;
        }
        if (so_error != 0) {
          result.ok = false;
          result.error_kind = classify_connect_errno(so_error);
          result.error_message = std::strerror(so_error);
          return result;
        }
        break;
      }
    }

    static const char kHttpRequest[] =
        "GET /v2/snaps?select=enabled HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n";

    size_t sent_total = 0;
    const size_t request_len = std::strlen(kHttpRequest);
#ifdef MSG_NOSIGNAL
    const int send_flags = MSG_NOSIGNAL;
#else
    const int send_flags = 0;
#endif
    while (sent_total < request_len) {
      const int wait_ms = remaining_ms(request_deadline);
      if (wait_ms <= 0) {
        result.ok = false;
        result.error_kind = SnapQueryErrorKind::kTimeout;
        result.error_message = "Timed out writing snapd HTTP request";
        return result;
      }
      struct pollfd pfd {};
      pfd.fd = fd.get();
      pfd.events = POLLOUT;
      const int ready = poll(&pfd, 1, wait_ms);
      if (ready < 0) {
        if (errno == EINTR) continue;
        result.ok = false;
        result.error_kind = SnapQueryErrorKind::kQueryFailed;
        result.error_message = std::strerror(errno);
        return result;
      }
      if (ready == 0) {
        result.ok = false;
        result.error_kind = SnapQueryErrorKind::kTimeout;
        result.error_message = "Timed out writing snapd HTTP request";
        return result;
      }
      const ssize_t written = send(fd.get(), kHttpRequest + sent_total,
                                   request_len - sent_total, send_flags);
      if (written < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        result.ok = false;
        result.error_kind = SnapQueryErrorKind::kQueryFailed;
        result.error_message = std::strerror(errno);
        return result;
      }
      if (written == 0) {
        result.ok = false;
        result.error_kind = SnapQueryErrorKind::kQueryFailed;
        result.error_message = "Unexpected EOF writing snapd HTTP request";
        return result;
      }
      sent_total += static_cast<size_t>(written);
    }

    std::string raw_response;
    char buffer[8192];
    const size_t max_raw_bytes =
        config_.max_response_bytes > std::numeric_limits<size_t>::max() - 65536
            ? std::numeric_limits<size_t>::max()
            : config_.max_response_bytes + 65536;

    while (true) {
      const int wait_ms = remaining_ms(request_deadline);
      if (wait_ms <= 0) {
        result.ok = false;
        result.error_kind = SnapQueryErrorKind::kTimeout;
        result.error_message = "Timed out reading snapd HTTP response";
        return result;
      }
      struct pollfd pfd {};
      pfd.fd = fd.get();
      pfd.events = POLLIN;
      const int ready = poll(&pfd, 1, wait_ms);
      if (ready < 0) {
        if (errno == EINTR) continue;
        result.ok = false;
        result.error_kind = SnapQueryErrorKind::kQueryFailed;
        result.error_message = std::strerror(errno);
        return result;
      }
      if (ready == 0) {
        result.ok = false;
        result.error_kind = SnapQueryErrorKind::kTimeout;
        result.error_message = "Timed out reading snapd HTTP response";
        return result;
      }
      const ssize_t count = recv(fd.get(), buffer, sizeof(buffer), 0);
      if (count < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        result.ok = false;
        result.error_kind = SnapQueryErrorKind::kQueryFailed;
        result.error_message = std::strerror(errno);
        return result;
      }
      if (count == 0) break;
      const size_t n = static_cast<size_t>(count);
      if (n > max_raw_bytes || raw_response.size() > max_raw_bytes - n) {
        result.ok = false;
        result.error_kind = SnapQueryErrorKind::kProtocolError;
        result.error_message = "Snapd response exceeded maximum allowed size";
        return result;
      }
      raw_response.append(buffer, n);
    }

    HttpResponse http;
    std::string http_error;
    if (!parse_http_response(raw_response, config_.max_response_bytes, &http,
                             &http_error)) {
      result.ok = false;
      result.error_kind = SnapQueryErrorKind::kProtocolError;
      result.error_message = http_error;
      return result;
    }

    JsonValue envelope;
    const bool json_ok = parse_json(http.body, &envelope) &&
                         envelope.type == JsonValue::Type::kObject;

    if (http.status_code == 401 || http.status_code == 403) {
      result.ok = false;
      result.error_kind = SnapQueryErrorKind::kPermissionDenied;
      if (json_ok) {
        const JsonValue* err_res = envelope.find("result");
        if (err_res != nullptr) {
          const auto msg = json_optional_string(*err_res, "message");
          if (msg.has_value() && !msg.value().empty()) {
            result.error_message = msg.value();
            return result;
          }
        }
      }
      result.error_message =
          "HTTP " + std::to_string(http.status_code) + " from snapd";
      return result;
    }

    if (!json_ok) {
      result.ok = false;
      result.error_kind = http.status_code == 200
                              ? SnapQueryErrorKind::kProtocolError
                              : SnapQueryErrorKind::kQueryFailed;
      result.error_message =
          http.status_code == 200
              ? "Malformed JSON in snapd response"
              : "HTTP " + std::to_string(http.status_code) + " from snapd";
      return result;
    }

    const OptionalString envelope_type = json_optional_string(envelope, "type");
    int envelope_status_code = 0;
    const bool has_status_code =
        json_non_negative_int(envelope, "status-code", &envelope_status_code);

    if (http.status_code != 200 || !has_status_code ||
        envelope_status_code != 200 || !envelope_type.has_value() ||
        envelope_type.value() != "sync") {
      result.ok = false;
      if (envelope_status_code == 401 || envelope_status_code == 403) {
        result.error_kind = SnapQueryErrorKind::kPermissionDenied;
      } else if (envelope_type.has_value() &&
                 envelope_type.value() == "error") {
        result.error_kind = SnapQueryErrorKind::kQueryFailed;
      } else {
        result.error_kind = SnapQueryErrorKind::kProtocolError;
      }

      const JsonValue* err_res = envelope.find("result");
      if (err_res != nullptr) {
        const auto msg = json_optional_string(*err_res, "message");
        if (msg.has_value() && !msg.value().empty()) {
          result.error_message = msg.value();
          return result;
        }
      }
      result.error_message = "Unexpected snapd response envelope";
      return result;
    }

    const JsonValue* items = envelope.find("result");
    if (items == nullptr || items->type != JsonValue::Type::kArray) {
      result.ok = false;
      result.error_kind = SnapQueryErrorKind::kProtocolError;
      result.error_message = "Missing result array in snapd response";
      return result;
    }

    result.snaps.reserve(items->array_value.size());
    for (const auto& entry : items->array_value) {
      if (entry.type != JsonValue::Type::kObject) {
        result.ok = false;
        result.error_kind = SnapQueryErrorKind::kProtocolError;
        result.error_message = "Non-object snap entry in snapd response";
        result.snaps.clear();
        return result;
      }

      SnapInstalledRecord record;
      const OptionalString raw_name = json_optional_string(entry, "name");
      const OptionalString raw_instance_name =
          json_optional_string(entry, "instance-name");
      const OptionalString raw_instance_key =
          json_optional_string(entry, "instance-key");

      if (raw_instance_name.has_value() && !raw_instance_name.value().empty()) {
        record.instance_name = raw_instance_name.value();
      } else if (raw_name.has_value()) {
        record.instance_name = raw_name.value();
      }
      if (raw_name.has_value()) {
        record.package_name = raw_name.value();
      }
      if (raw_instance_key.has_value() && !raw_instance_key.value().empty()) {
        record.instance_key = raw_instance_key;
      }

      const OptionalString type = json_optional_string(entry, "type");
      if (type.has_value()) {
        record.type = type.value();
      }
      record.status = json_optional_string(entry, "status");
      record.title = json_optional_string(entry, "title");
      record.version = json_optional_string(entry, "version");
      record.revision = json_optional_string_or_number(entry, "revision");

      const OptionalString tracking =
          json_optional_string(entry, "tracking-channel");
      if (tracking.has_value() && !tracking.value().empty()) {
        record.channel = tracking;
      } else {
        record.channel = json_optional_string(entry, "channel");
      }

      record.summary = json_optional_string(entry, "summary");
      record.description = json_optional_string(entry, "description");
      record.license = json_optional_string(entry, "license");

      result.snaps.push_back(std::move(record));
    }

    result.ok = true;
    result.error_kind = SnapQueryErrorKind::kNone;
    return result;
  }

 private:
  SnapdSocketConfig config_;
};

}  // namespace

std::unique_ptr<SnapBackend> create_snapd_socket_backend(
    SnapdSocketConfig config) {
  return std::unique_ptr<SnapBackend>(
      new SnapdSocketBackend(std::move(config)));
}

}  // namespace flutter_device_apps_linux
