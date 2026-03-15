#pragma once

#include <optional>
#include <string>

std::string json_escape(const std::string& input);
std::string json_string(const std::string& input);
std::string json_nullable_string(const std::optional<std::string>& input);
std::string json_unescape(const std::string& input);

std::string json_read_required_string(const std::string& json, const std::string& key);
std::optional<std::string> json_read_optional_string(const std::string& json, const std::string& key);
bool json_read_required_bool(const std::string& json, const std::string& key);
long long json_read_required_integer(const std::string& json, const std::string& key);
