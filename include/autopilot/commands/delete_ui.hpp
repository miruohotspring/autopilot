#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

std::optional<std::string> select_project_to_delete(const std::set<std::string>& projects);
std::optional<std::string> select_project_to_add_path(const std::set<std::string>& projects);
std::optional<std::string> select_project_to_remove_path(const std::set<std::string>& projects);
std::optional<std::string> select_path_to_remove(const std::vector<std::string>& paths);
std::optional<bool> confirm_delete(const std::string& project_name);
std::optional<bool> confirm_remove_path(const std::string& path_value);
