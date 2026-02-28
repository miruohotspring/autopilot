#pragma once

#include <filesystem>
#include <set>
#include <string>

bool is_valid_project_name(const std::string& name);
std::set<std::string> load_top_level_projects(const std::filesystem::path& projects_file);
bool file_ends_with_newline(const std::filesystem::path& file);
bool remove_project_block(const std::filesystem::path& projects_file, const std::string& project_name);
