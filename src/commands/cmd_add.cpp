#include "autopilot/commands/cmd_add.hpp"
#include "autopilot/commands/delete_ui.hpp"
#include "autopilot/projects/project_paths.hpp"
#include "autopilot/projects/project_store.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace fs = std::filesystem;

int cmd_add(const std::string& path_arg, const std::optional<std::string>& maybe_project_name) {
  if (maybe_project_name.has_value() && !is_valid_project_name(*maybe_project_name)) {
    std::cerr << "invalid project name\n";
    return 1;
  }

  if (!autopilot_dir_exists()) {
    std::cerr << "Please run ap init first\n";
    return 1;
  }

  const fs::path projects_file = projects_file_path();
  try {
    if (!fs::exists(projects_file)) {
      std::cerr << "project not found\n";
      return 1;
    }

    const auto existing_projects = load_top_level_projects(projects_file);
    if (existing_projects.empty()) {
      std::cerr << "project not found\n";
      return 1;
    }

    std::string project_name;
    if (maybe_project_name.has_value()) {
      project_name = *maybe_project_name;
      if (existing_projects.find(project_name) == existing_projects.end()) {
        std::cerr << "project not found\n";
        return 1;
      }
    } else {
      const std::optional<std::string> selected_project = select_project_to_add_path(existing_projects);
      if (!selected_project.has_value()) {
        std::cerr << "failed to read project selection\n";
        return 1;
      }
      project_name = *selected_project;
    }

    const fs::path absolute_path = fs::absolute(fs::path(path_arg)).lexically_normal();
    std::string normalized_path = absolute_path.string();
    while (normalized_path.size() > 1 && normalized_path.back() == fs::path::preferred_separator) {
      normalized_path.pop_back();
    }
    const AddProjectPathResult result = add_project_path(projects_file, project_name, normalized_path);
    if (result == AddProjectPathResult::ProjectNotFound) {
      std::cerr << "project not found\n";
      return 1;
    }
    if (result == AddProjectPathResult::AlreadyExists) {
      std::cout << "path already exists: " << normalized_path << '\n';
      return 0;
    }

    std::cout << "added path: " << normalized_path << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap add failed: " << e.what() << '\n';
    return 1;
  }
}
