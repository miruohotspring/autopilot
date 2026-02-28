#pragma once

#include <optional>
#include <set>
#include <string>

std::optional<std::string> select_project_to_delete(const std::set<std::string>& projects);
std::optional<std::string> select_project_to_add_path(const std::set<std::string>& projects);
std::optional<bool> confirm_delete(const std::string& project_name);
