#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

enum class AddProjectPathResult {
  Added,
  AlreadyExists,
  PathAlreadyExistsWithDifferentName,
  NameAlreadyExistsWithDifferentPath,
  ProjectNotFound,
};

enum class RemoveProjectPathResult {
  Removed,
  PathNotFound,
  ProjectNotFound,
};

bool is_valid_project_name(const std::string& name);
std::set<std::string> load_top_level_projects(const std::filesystem::path& projects_file);
std::vector<std::string> load_project_paths(
    const std::filesystem::path& projects_file,
    const std::string& project_name);
bool project_has_path_name(
    const std::filesystem::path& projects_file,
    const std::string& project_name,
    const std::string& path_name);
bool file_ends_with_newline(const std::filesystem::path& file);
bool remove_project_block(const std::filesystem::path& projects_file, const std::string& project_name);
AddProjectPathResult add_project_path(
    const std::filesystem::path& projects_file,
    const std::string& project_name,
    const std::string& path_value,
    const std::string& path_name);
RemoveProjectPathResult remove_project_path(
    const std::filesystem::path& projects_file,
    const std::string& project_name,
    const std::string& path_value);
