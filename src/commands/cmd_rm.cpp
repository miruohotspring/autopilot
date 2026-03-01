#include "autopilot/commands/cmd_rm.hpp"
#include "autopilot/commands/delete_ui.hpp"
#include "autopilot/projects/project_paths.hpp"
#include "autopilot/projects/project_store.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int cmd_rm(const std::optional<std::string>& maybe_project_name) {
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
      const std::optional<std::string> selected_project =
          select_project_to_remove_path(existing_projects);
      if (!selected_project.has_value()) {
        std::cerr << "failed to read project selection\n";
        return 1;
      }
      project_name = *selected_project;
    }

    const std::vector<std::string> paths = load_project_paths(projects_file, project_name);
    if (paths.empty()) {
      std::cerr << "path not found\n";
      return 1;
    }

    const std::optional<std::string> selected_path = select_path_to_remove(paths);
    if (!selected_path.has_value()) {
      std::cerr << "failed to read path selection\n";
      return 1;
    }

    const std::optional<bool> confirmed = confirm_remove_path(*selected_path);
    if (!confirmed.has_value()) {
      std::cerr << "failed to read confirmation\n";
      return 1;
    }
    if (!*confirmed) {
      std::cout << "canceled\n";
      return 1;
    }

    const RemoveProjectPathResult result =
        remove_project_path(projects_file, project_name, *selected_path);
    if (result == RemoveProjectPathResult::ProjectNotFound) {
      std::cerr << "project not found\n";
      return 1;
    }
    if (result == RemoveProjectPathResult::PathNotFound) {
      std::cerr << "path not found\n";
      return 1;
    }

    std::cout << "removed path: " << *selected_path << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap rm failed: " << e.what() << '\n';
    return 1;
  }
}
