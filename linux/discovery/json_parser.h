#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_JSON_PARSER_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_JSON_PARSER_H_

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "discovered_application.h"

namespace flutter_device_apps_linux {

// Strict C++14 RFC 8259 JSON DOM value used by local providers (such as
// snapd HTTP responses and Nix profile manifest.json files).
struct JsonValue {
  enum class Type {
    kNull,
    kBool,
    kNumber,
    kString,
    kArray,
    kObject,
  };

  Type type = Type::kNull;
  bool bool_value = false;
  std::string string_value;  // Also holds raw numeric token for kNumber.
  std::vector<JsonValue> array_value;
  std::map<std::string, JsonValue> object_value;

  const JsonValue* find(const std::string& key) const;
};

// Parses a complete RFC 8259 JSON document with a hard nesting-depth cap.
// Rejects trailing non-whitespace characters, invalid escapes/surrogates,
// control characters in strings, and excessive nesting depth.
bool parse_json(const std::string& input, JsonValue* out,
                size_t max_depth = 64);

// Helper extractors for JSON object fields.
OptionalString json_optional_string(const JsonValue& object,
                                    const std::string& key);
OptionalString json_optional_string_or_number(const JsonValue& object,
                                              const std::string& key);
bool json_non_negative_int(const JsonValue& object, const std::string& key,
                           int* out);
bool json_signed_int(const JsonValue& object, const std::string& key,
                     int* out);

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_JSON_PARSER_H_
