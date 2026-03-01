#include "autopilot/commands/cmd_delete.hpp"
#include "autopilot/commands/delete_ui.hpp"
#include "autopilot/projects/project_paths.hpp"
#include "autopilot/projects/project_store.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace fs = std::filesystem;

int cmd_delete(const std::optional<std::string>& maybe_project_name) {
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
          select_project_to_delete(existing_projects);
      if (!selected_project.has_value()) {
        std::cerr << "failed to read project selection\n";
        return 1;
      }
      project_name = *selected_project;
    }

    const std::optional<bool> confirmed = confirm_delete(project_name);
    if (!confirmed.has_value()) {
      std::cerr << "failed to read confirmation\n";
      return 1;
    }
    if (!*confirmed) {
      std::cout << "canceled\n";
      return 1;
    }

    if (!remove_project_block(projects_file, project_name)) {
      std::cerr << "project not found\n";
      return 1;
    }

    std::cout << "deleted project: " << project_name << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap delete failed: " << e.what() << '\n';
    return 1;
  }
}
