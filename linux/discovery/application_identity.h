#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_IDENTITY_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_IDENTITY_H_

#include <string>

#include "discovered_application.h"

namespace flutter_device_apps_linux {

// Returns the stable canonical token for an InstallationScope value.
const char* installation_scope_token(InstallationScope scope);

// Builds a deterministic, collision-free, provider-scoped application ID.
//
// Encoding rules:
// - Each variable component percent-escapes '%', ':', and ASCII control bytes,
//   so literal ':' delimiters unambiguously separate components regardless of
//   embedded delimiters in provider or native identifiers.
// - Two components ("<provider>:<native>") are emitted when installation scope
//   is kUnknown and installation_id is unset.
// - Three components ("<provider>:<native>:<scope>") are emitted when scope is
//   known and installation_id is unset.
// - Four components ("<provider>:<native>:<scope>:<installation>") are emitted
//   when installation_id is present (including empty string), keeping missing
//   and empty installation IDs distinct.
std::string build_application_id(
    const std::string& provider_id, const std::string& native_id,
    InstallationScope scope = InstallationScope::kUnknown,
    const OptionalString& installation_id = OptionalString::none());

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_APPLICATION_IDENTITY_H_
