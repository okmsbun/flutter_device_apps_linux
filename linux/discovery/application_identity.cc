#include "application_identity.h"

namespace flutter_device_apps_linux {
namespace {

std::string escape_identity_component(const std::string& input) {
  static const char kHexDigits[] = "0123456789ABCDEF";
  std::string escaped;
  escaped.reserve(input.size());
  for (unsigned char ch : input) {
    if (ch == '%' || ch == ':' || ch < 0x20 || ch == 0x7F) {
      escaped.push_back('%');
      escaped.push_back(kHexDigits[(ch >> 4) & 0x0F]);
      escaped.push_back(kHexDigits[ch & 0x0F]);
    } else {
      escaped.push_back(static_cast<char>(ch));
    }
  }
  return escaped;
}

}  // namespace

const char* installation_scope_token(InstallationScope scope) {
  switch (scope) {
    case InstallationScope::kUnknown:
      return "unknown";
    case InstallationScope::kSystem:
      return "system";
    case InstallationScope::kUser:
      return "user";
    case InstallationScope::kOther:
      return "other";
  }
  return "unknown";
}

std::string build_application_id(const std::string& provider_id,
                                 const std::string& native_id,
                                 InstallationScope scope,
                                 const OptionalString& installation_id) {
  std::string id = escape_identity_component(provider_id);
  id.push_back(':');
  id.append(escape_identity_component(native_id));

  if (installation_id.has_value()) {
    id.push_back(':');
    id.append(installation_scope_token(scope));
    id.push_back(':');
    id.append(escape_identity_component(installation_id.value()));
  } else if (scope != InstallationScope::kUnknown) {
    id.push_back(':');
    id.append(installation_scope_token(scope));
  }
  return id;
}

}  // namespace flutter_device_apps_linux
