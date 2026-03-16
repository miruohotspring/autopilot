#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct ProjectPathEntry {
  std::string name;
  std::string path;
};

struct ProjectConfig {
  std::string name;
  std::string slug;
  std::vector<std::string> paths;
};

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
bool is_valid_project_slug(const std::string& slug);
std::set<std::string> load_top_level_projects(const std::filesystem::path& projects_file);
std::optional<ProjectConfig> load_project_config(
    const std::filesystem::path& projects_file,
    const std::string& project_name);
ProjectConfig load_required_project_config(
    const std::filesystem::path& projects_file,
    const std::string& project_name);
void append_project_config(const std::filesystem::path& projects_file, const ProjectConfig& config);
std::vector<ProjectPathEntry> load_project_path_entries(
    const std::filesystem::path& projects_file,
    const std::string& project_name);
std::vector<std::string> load_project_paths(
    const std::filesystem::path& projects_file,
    const std::string& project_name);
bool project_has_path_name(
    const std::filesystem::path& projects_file,
    const std::string& project_name,
    const std::string& path_name);
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
