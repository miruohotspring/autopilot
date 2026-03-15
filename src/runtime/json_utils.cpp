#include "autopilot/runtime/json_utils.hpp"

#include <cctype>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

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

std::regex json_array_field_regex(const std::string& key) {
  return std::regex(
      "\"" + key + "\"\\s*:\\s*\\[((?:.|\\n|\\r)*?)\\]",
      std::regex_constants::ECMAScript);
}

std::regex json_field_presence_regex(const std::string& key) {
  return std::regex("\"" + key + "\"\\s*:", std::regex_constants::ECMAScript);
}

bool json_field_present(const std::string& json, const std::string& key) {
  return regex_search_single(json, json_field_presence_regex(key)).has_value();
}

void skip_ascii_whitespace(const std::string& input, std::size_t& pos) {
  while (pos < input.size() &&
         std::isspace(static_cast<unsigned char>(input[pos])) != 0) {
    ++pos;
  }
}

std::vector<std::string> parse_json_string_array_body(const std::string& body) {
  std::vector<std::string> values;
  std::size_t pos = 0;
  while (true) {
    skip_ascii_whitespace(body, pos);
    if (pos >= body.size()) {
      break;
    }
    if (body[pos] != '"') {
      throw std::runtime_error("invalid string array");
    }

    ++pos;
    std::string current;
    bool escaped = false;
    while (pos < body.size()) {
      const char ch = body[pos++];
      if (escaped) {
        current.push_back('\\');
        current.push_back(ch);
        escaped = false;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        continue;
      }
      if (ch == '"') {
        break;
      }
      current.push_back(ch);
    }
    if (escaped) {
      throw std::runtime_error("invalid string array");
    }
    values.push_back(json_unescape(current));

    skip_ascii_whitespace(body, pos);
    if (pos >= body.size()) {
      break;
    }
    if (body[pos] != ',') {
      throw std::runtime_error("invalid string array");
    }
    ++pos;
  }
  return values;
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

std::string json_string_array(const std::vector<std::string>& input) {
  std::string out = "[";
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }
    out += json_string(input[i]);
  }
  out += "]";
  return out;
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
  const std::optional<std::smatch> match = regex_search_single(json, json_string_field_regex(key));
  if (!match.has_value()) {
    if (json_field_present(json, key)) {
      throw std::runtime_error("invalid string field: " + key);
    }
    return std::nullopt;
  }
  return json_unescape((*match)[1].str());
}

std::vector<std::string> json_read_required_string_array(const std::string& json, const std::string& key) {
  const std::optional<std::smatch> match = regex_search_single(json, json_array_field_regex(key));
  if (!match.has_value()) {
    throw std::runtime_error("missing string array field: " + key);
  }
  return parse_json_string_array_body((*match)[1].str());
}

std::optional<std::vector<std::string>> json_read_optional_string_array(
    const std::string& json,
    const std::string& key) {
  const std::optional<std::smatch> match = regex_search_single(json, json_array_field_regex(key));
  if (!match.has_value()) {
    if (json_field_present(json, key)) {
      throw std::runtime_error("invalid string array field: " + key);
    }
    return std::nullopt;
  }
  return parse_json_string_array_body((*match)[1].str());
}

bool json_read_required_bool(const std::string& json, const std::string& key) {
  const std::optional<std::smatch> match = regex_search_single(json, json_bool_field_regex(key));
  if (!match.has_value()) {
    throw std::runtime_error("missing bool field: " + key);
  }
  return (*match)[1].str() == "true";
}

std::optional<bool> json_read_optional_bool(const std::string& json, const std::string& key) {
  const std::optional<std::smatch> match = regex_search_single(json, json_bool_field_regex(key));
  if (!match.has_value()) {
    if (json_field_present(json, key)) {
      throw std::runtime_error("invalid bool field: " + key);
    }
    return std::nullopt;
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

std::optional<long long> json_read_optional_integer(const std::string& json, const std::string& key) {
  const std::optional<std::smatch> match = regex_search_single(json, json_integer_field_regex(key));
  if (!match.has_value()) {
    if (json_field_present(json, key)) {
      throw std::runtime_error("invalid integer field: " + key);
    }
    return std::nullopt;
  }
  return std::stoll((*match)[1].str());
}
