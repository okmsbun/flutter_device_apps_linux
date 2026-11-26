#include "json_parser.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace flutter_device_apps_linux {
namespace {

bool parse_strict_unsigned(const std::string& text, size_t* out) {
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

class JsonParser {
 public:
  JsonParser(const std::string& input, size_t max_depth)
      : input_(input), max_depth_(max_depth), pos_(0) {}

  bool parse(JsonValue* out) {
    if (out == nullptr) return false;
    skip_whitespace();
    if (!parse_value(out, 0)) return false;
    skip_whitespace();
    return pos_ == input_.size();
  }

 private:
  void skip_whitespace() {
    while (pos_ < input_.size()) {
      const char ch = input_[pos_];
      if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool parse_value(JsonValue* out, size_t depth) {
    if (depth > max_depth_ || pos_ >= input_.size()) return false;
    const char ch = input_[pos_];
    if (ch == '{') return parse_object(out, depth + 1);
    if (ch == '[') return parse_array(out, depth + 1);
    if (ch == '"') {
      out->type = JsonValue::Type::kString;
      return parse_string(&out->string_value);
    }
    if (ch == 't') {
      return parse_literal("true", JsonValue::Type::kBool, true, out);
    }
    if (ch == 'f') {
      return parse_literal("false", JsonValue::Type::kBool, false, out);
    }
    if (ch == 'n') {
      return parse_literal("null", JsonValue::Type::kNull, false, out);
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) return parse_number(out);
    return false;
  }

  bool parse_literal(const char* literal, JsonValue::Type type, bool bool_val,
                     JsonValue* out) {
    const size_t len = std::strlen(literal);
    if (input_.size() - pos_ < len ||
        input_.compare(pos_, len, literal) != 0) {
      return false;
    }
    pos_ += len;
    out->type = type;
    out->bool_value = bool_val;
    return true;
  }

  bool parse_hex4(uint32_t* out) {
    if (input_.size() - pos_ < 4) return false;
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const unsigned char ch = static_cast<unsigned char>(input_[pos_++]);
      uint32_t digit = 0;
      if (ch >= '0' && ch <= '9') {
        digit = static_cast<uint32_t>(ch - '0');
      } else if (ch >= 'a' && ch <= 'f') {
        digit = static_cast<uint32_t>(10 + (ch - 'a'));
      } else if (ch >= 'A' && ch <= 'F') {
        digit = static_cast<uint32_t>(10 + (ch - 'A'));
      } else {
        return false;
      }
      value = (value << 4) | digit;
    }
    *out = value;
    return true;
  }

  static void append_utf8(uint32_t codepoint, std::string* out) {
    if (codepoint <= 0x7F) {
      out->push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
      out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
      out->push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
      out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
      out->push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
      out->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
  }

  bool parse_string(std::string* out) {
    out->clear();
    if (pos_ >= input_.size() || input_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < input_.size()) {
      const unsigned char ch = static_cast<unsigned char>(input_[pos_++]);
      if (ch == '"') return true;
      if (ch < 0x20) return false;
      if (ch != '\\') {
        out->push_back(static_cast<char>(ch));
        continue;
      }
      if (pos_ >= input_.size()) return false;
      const char esc = input_[pos_++];
      switch (esc) {
        case '"':
          out->push_back('"');
          break;
        case '\\':
          out->push_back('\\');
          break;
        case '/':
          out->push_back('/');
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          uint32_t code = 0;
          if (!parse_hex4(&code)) return false;
          if (code >= 0xD800 && code <= 0xDBFF) {
            if (input_.size() - pos_ < 2 || input_[pos_] != '\\' ||
                input_[pos_ + 1] != 'u') {
              return false;
            }
            pos_ += 2;
            uint32_t low = 0;
            if (!parse_hex4(&low) || low < 0xDC00 || low > 0xDFFF) {
              return false;
            }
            const uint32_t combined =
                0x10000 + (((code - 0xD800) << 10) | (low - 0xDC00));
            append_utf8(combined, out);
          } else if (code >= 0xDC00 && code <= 0xDFFF) {
            return false;
          } else {
            append_utf8(code, out);
          }
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool parse_number(JsonValue* out) {
    const size_t start = pos_;
    if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
    if (pos_ >= input_.size()) return false;
    if (input_[pos_] == '0') {
      ++pos_;
    } else if (input_[pos_] >= '1' && input_[pos_] <= '9') {
      while (pos_ < input_.size() && input_[pos_] >= '0' &&
             input_[pos_] <= '9') {
        ++pos_;
      }
    } else {
      return false;
    }

    if (pos_ < input_.size() && input_[pos_] == '.') {
      ++pos_;
      if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
        return false;
      }
      while (pos_ < input_.size() && input_[pos_] >= '0' &&
             input_[pos_] <= '9') {
        ++pos_;
      }
    }

    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < input_.size() &&
          (input_[pos_] == '+' || input_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
        return false;
      }
      while (pos_ < input_.size() && input_[pos_] >= '0' &&
             input_[pos_] <= '9') {
        ++pos_;
      }
    }

    out->type = JsonValue::Type::kNumber;
    out->string_value = input_.substr(start, pos_ - start);
    return true;
  }

  bool parse_array(JsonValue* out, size_t depth) {
    out->type = JsonValue::Type::kArray;
    out->array_value.clear();
    ++pos_;  // '['
    skip_whitespace();
    if (pos_ < input_.size() && input_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (pos_ < input_.size()) {
      JsonValue element;
      if (!parse_value(&element, depth)) return false;
      out->array_value.push_back(std::move(element));
      skip_whitespace();
      if (pos_ >= input_.size()) return false;
      if (input_[pos_] == ']') {
        ++pos_;
        return true;
      }
      if (input_[pos_] != ',') return false;
      ++pos_;
      skip_whitespace();
    }
    return false;
  }

  bool parse_object(JsonValue* out, size_t depth) {
    out->type = JsonValue::Type::kObject;
    out->object_value.clear();
    ++pos_;  // '{'
    skip_whitespace();
    if (pos_ < input_.size() && input_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (pos_ < input_.size()) {
      std::string key;
      if (!parse_string(&key)) return false;
      skip_whitespace();
      if (pos_ >= input_.size() || input_[pos_] != ':') return false;
      ++pos_;
      skip_whitespace();
      JsonValue value;
      if (!parse_value(&value, depth)) return false;
      out->object_value[std::move(key)] = std::move(value);
      skip_whitespace();
      if (pos_ >= input_.size()) return false;
      if (input_[pos_] == '}') {
        ++pos_;
        return true;
      }
      if (input_[pos_] != ',') return false;
      ++pos_;
      skip_whitespace();
    }
    return false;
  }

  const std::string& input_;
  size_t max_depth_;
  size_t pos_;
};

}  // namespace

const JsonValue* JsonValue::find(const std::string& key) const {
  if (type != Type::kObject) return nullptr;
  const auto it = object_value.find(key);
  return it == object_value.end() ? nullptr : &it->second;
}

bool parse_json(const std::string& input, JsonValue* out, size_t max_depth) {
  JsonParser parser(input, max_depth);
  return parser.parse(out);
}

OptionalString json_optional_string(const JsonValue& object,
                                    const std::string& key) {
  const JsonValue* value = object.find(key);
  if (value == nullptr || value->type != JsonValue::Type::kString) {
    return OptionalString::none();
  }
  return OptionalString::of(value->string_value);
}

OptionalString json_optional_string_or_number(const JsonValue& object,
                                              const std::string& key) {
  const JsonValue* value = object.find(key);
  if (value == nullptr) return OptionalString::none();
  if (value->type == JsonValue::Type::kString ||
      value->type == JsonValue::Type::kNumber) {
    return OptionalString::of(value->string_value);
  }
  return OptionalString::none();
}

bool json_non_negative_int(const JsonValue& object, const std::string& key,
                           int* out) {
  if (out == nullptr) return false;
  const JsonValue* value = object.find(key);
  if (value == nullptr || value->type != JsonValue::Type::kNumber) {
    return false;
  }
  size_t parsed = 0;
  if (!parse_strict_unsigned(value->string_value, &parsed) ||
      parsed > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  *out = static_cast<int>(parsed);
  return true;
}

bool json_signed_int(const JsonValue& object, const std::string& key,
                     int* out) {
  if (out == nullptr) return false;
  const JsonValue* value = object.find(key);
  if (value == nullptr || value->type != JsonValue::Type::kNumber ||
      value->string_value.empty()) {
    return false;
  }
  const std::string& raw = value->string_value;
  bool negative = false;
  size_t start = 0;
  if (raw[0] == '-') {
    negative = true;
    start = 1;
  }
  size_t magnitude = 0;
  if (!parse_strict_unsigned(raw.substr(start), &magnitude)) {
    return false;
  }
  if (!negative) {
    if (magnitude > static_cast<size_t>(std::numeric_limits<int>::max())) {
      return false;
    }
    *out = static_cast<int>(magnitude);
    return true;
  }
  const size_t min_abs =
      static_cast<size_t>(std::numeric_limits<int>::max()) + 1;
  if (magnitude > min_abs) return false;
  if (magnitude == min_abs) {
    *out = std::numeric_limits<int>::min();
    return true;
  }
  *out = -static_cast<int>(magnitude);
  return true;
}

}  // namespace flutter_device_apps_linux
