#include "autopilot/runtime/json_utils.hpp"

#include <regex>
#include <stdexcept>
#include <string>

namespace {

std::optional<std::smatch> regex_search_single(const std::string& input, const std::regex& re) {
  std::smatch match;
  if (std::regex_search(input, match, re)) {
    return match;
  }
  return std::nullopt;
}

std::regex json_string_field_regex(const std::string& key) {
  return std::regex(
      "\"" + key + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"",
      std::regex_constants::ECMAScript);
}

std::regex json_optional_string_field_regex(const std::string& key) {
  return std::regex("\"" + key + "\"\\s*:\\s*null", std::regex_constants::ECMAScript);
}

std::regex json_bool_field_regex(const std::string& key) {
  return std::regex(
      "\"" + key + "\"\\s*:\\s*(true|false)",
      std::regex_constants::ECMAScript);
}

std::regex json_integer_field_regex(const std::string& key) {
  return std::regex(
      "\"" + key + "\"\\s*:\\s*(-?[0-9]+)",
      std::regex_constants::ECMAScript);
}

} // namespace

std::string json_escape(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (char ch : input) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::string json_string(const std::string& input) {
  return "\"" + json_escape(input) + "\"";
}

std::string json_nullable_string(const std::optional<std::string>& input) {
  return input.has_value() ? json_string(*input) : "null";
}

std::string json_unescape(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  bool escaped = false;
  for (char ch : input) {
    if (!escaped) {
      if (ch == '\\') {
        escaped = true;
      } else {
        out.push_back(ch);
      }
      continue;
    }

    switch (ch) {
      case '\\':
        out.push_back('\\');
        break;
      case '"':
        out.push_back('"');
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      default:
        out.push_back(ch);
        break;
    }
    escaped = false;
  }
  if (escaped) {
    out.push_back('\\');
  }
  return out;
}

std::string json_read_required_string(const std::string& json, const std::string& key) {
  const std::optional<std::smatch> match = regex_search_single(json, json_string_field_regex(key));
  if (!match.has_value()) {
    throw std::runtime_error("missing string field: " + key);
  }
  return json_unescape((*match)[1].str());
}

std::optional<std::string> json_read_optional_string(const std::string& json, const std::string& key) {
  if (regex_search_single(json, json_optional_string_field_regex(key)).has_value()) {
    return std::nullopt;
  }
  return json_read_required_string(json, key);
}

bool json_read_required_bool(const std::string& json, const std::string& key) {
  const std::optional<std::smatch> match = regex_search_single(json, json_bool_field_regex(key));
  if (!match.has_value()) {
    throw std::runtime_error("missing bool field: " + key);
  }
  return (*match)[1].str() == "true";
}

long long json_read_required_integer(const std::string& json, const std::string& key) {
  const std::optional<std::smatch> match = regex_search_single(json, json_integer_field_regex(key));
  if (!match.has_value()) {
    throw std::runtime_error("missing integer field: " + key);
  }
  return std::stoll((*match)[1].str());
}
