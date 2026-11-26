#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_DISCOVERED_APPLICATION_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_DISCOVERED_APPLICATION_H_

#include <string>
#include <utility>

namespace flutter_device_apps_linux {

// Minimal C++14-compatible optional value wrapper.
// Explicitly distinguishes a missing value from a present (possibly empty)
// value without relying on C++17 std::optional or external dependencies.
template <typename T>
class OptionalValue {
 public:
  OptionalValue() : has_value_(false), value_() {}
  explicit OptionalValue(T value)
      : has_value_(true), value_(std::move(value)) {}

  static OptionalValue none() { return OptionalValue(); }
  static OptionalValue of(T value) { return OptionalValue(std::move(value)); }

  bool has_value() const { return has_value_; }
  const T& value() const { return value_; }
  T& value() { return value_; }

  void set(T value) {
    value_ = std::move(value);
    has_value_ = true;
  }

  void reset() {
    value_ = T();
    has_value_ = false;
  }

  bool operator==(const OptionalValue& other) const {
    return has_value_ == other.has_value_ &&
           (!has_value_ || value_ == other.value_);
  }

  bool operator!=(const OptionalValue& other) const {
    return !(*this == other);
  }

 private:
  bool has_value_;
  T value_;
};

using OptionalString = OptionalValue<std::string>;

// Tri-state capability value that keeps unknown distinct from known false.
enum class CapabilityState {
  kUnknown,
  kSupported,
  kUnsupported,
};

inline CapabilityState capability_from_bool(bool supported) {
  return supported ? CapabilityState::kSupported
                   : CapabilityState::kUnsupported;
}

// Scope in which an application is installed.
enum class InstallationScope {
  kUnknown,
  kSystem,
  kUser,
  kOther,
};

// Known packaging or installation source, kept distinct from the discovery
// provider (for example, GIO may discover a Flatpak-installed application).
enum class InstallationSource {
  kUnknown,
  kFlatpak,
  kSnap,
  kAppImage,
  kDpkg,
  kRpm,
  kPacman,
  kNix,
};

// Internal classification of a discovered software unit.
// Must remain kUnknown when reliable evidence is unavailable.
enum class ApplicationClassification {
  kUnknown,
  kGuiApplication,
  kCliApplication,
  kService,
  kLibrary,
  kRuntime,
};

// Desktop menu visibility state (for example, as reported by GIO's
// g_app_info_should_show()). Kept distinct from installation status,
// launchability, and application classification.
enum class DesktopMenuVisibility {
  kUnknown,
  kVisible,
  kNotVisible,
};

// Provenance describing where a discovery record originated.
struct SourceProvenance {
  // Required identifier of the provider that discovered this record.
  std::string provider_id;

  // Required provider-native identifier before provider scoping.
  std::string native_id;

  // Known installation source, or kUnknown when not reliably determined.
  InstallationSource installation_source = InstallationSource::kUnknown;

  // Known installation scope, or kUnknown when not reliably determined.
  InstallationScope installation_scope = InstallationScope::kUnknown;
};

// Optional descriptive metadata owned by the internal discovery record.
struct ApplicationMetadata {
  OptionalString name;
  OptionalString version;
  OptionalString revision;
  OptionalString channel;
  OptionalString summary;
  OptionalString description;
  OptionalString executable;
  OptionalString homepage;
  OptionalString license;
  OptionalString icon_reference;
  OptionalString architecture;
  OptionalString branch;
};

// Tri-state capability flags owned by the internal discovery record.
struct ApplicationCapabilities {
  CapabilityState supports_files = CapabilityState::kUnknown;
  CapabilityState supports_uris = CapabilityState::kUnknown;
  CapabilityState requires_terminal = CapabilityState::kUnknown;
  CapabilityState launchable = CapabilityState::kUnknown;
};

// Unified internal representation of a discovered application.
// Owns all data directly and stays independent of Flutter, GTK, GIO, and
// provider-specific native handles so records can later cross worker threads.
struct DiscoveredApplication {
  // Required deterministic, provider-scoped application identifier.
  std::string application_id;

  // Discovery provenance (provider ID, provider-native ID, installation source,
  // and installation scope).
  SourceProvenance provenance;

  // Optional desktop entry integration identifier (for example,
  // "org.example.Editor.desktop"). Unset when no desktop entry exists.
  OptionalString desktop_file_id;

  // Optional package identifier (for example, Flatpak app ID or DPKG package).
  OptionalString package_id;

  // Optional installation-specific identity used to distinguish multiple
  // installations of the same application within a provider.
  OptionalString installation_id;

  // Optional descriptive metadata.
  ApplicationMetadata metadata;

  // Tri-state application capabilities.
  ApplicationCapabilities capabilities;

  // Application classification; defaults to kUnknown.
  ApplicationClassification classification =
      ApplicationClassification::kUnknown;

  // Desktop menu visibility state; defaults to kUnknown for providers without
  // desktop menu visibility semantics.
  DesktopMenuVisibility desktop_menu_visibility =
      DesktopMenuVisibility::kUnknown;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_DISCOVERED_APPLICATION_H_
