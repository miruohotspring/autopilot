#include "autopilot/commands/cmd_add.hpp"
#include "autopilot/commands/delete_ui.hpp"
#include "autopilot/projects/project_paths.hpp"
#include "autopilot/projects/project_store.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string trim_ascii_whitespace(const std::string& s) {
  const std::size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

bool is_valid_path_name(const std::string& path_name) {
  if (path_name.empty()) {
    return false;
  }
  if (path_name == "." || path_name == "..") {
    return false;
  }
  return path_name.find('/') == std::string::npos && path_name.find('\\') == std::string::npos;
}

bool is_same_symlink_target(const fs::path& link_path, const fs::path& expected_target) {
  if (!fs::is_symlink(link_path)) {
    return false;
  }
  fs::path current_target = fs::read_symlink(link_path);
  if (current_target.is_relative()) {
    current_target = fs::absolute(link_path.parent_path() / current_target).lexically_normal();
  } else {
    current_target = fs::absolute(current_target).lexically_normal();
  }
  return current_target == expected_target.lexically_normal();
}

} // namespace

int cmd_add(
    const std::string& path_arg,
    const std::optional<std::string>& maybe_path_name,
    const std::optional<std::string>& maybe_project_name) {
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
          select_project_to_add_path(existing_projects);
      if (!selected_project.has_value()) {
        std::cerr << "failed to read project selection\n";
        return 1;
      }
      project_name = *selected_project;
    }

    std::string path_name;
    if (maybe_path_name.has_value()) {
      path_name = *maybe_path_name;
    } else {
      const bool has_main_name = project_has_path_name(projects_file, project_name, "main");
      std::cout << "Enter path name [main]: ";
      std::cout.flush();
      std::string input_path_name;
      if (!std::getline(std::cin, input_path_name)) {
        std::cerr << "failed to read path name\n";
        return 1;
      }
      input_path_name = trim_ascii_whitespace(input_path_name);
      if (input_path_name.empty()) {
        if (has_main_name) {
          std::cerr << "path name is required because 'main' already exists\n";
          return 1;
        }
        path_name = "main";
      } else {
        path_name = input_path_name;
      }
    }
    if (!is_valid_path_name(path_name)) {
      std::cerr << "invalid path name\n";
      return 1;
    }

    const fs::path absolute_path = fs::absolute(fs::path(path_arg)).lexically_normal();
    std::string normalized_path = absolute_path.string();
    while (normalized_path.size() > 1 && normalized_path.back() == fs::path::preferred_separator) {
      normalized_path.pop_back();
    }
    const AddProjectPathResult result =
        add_project_path(projects_file, project_name, normalized_path, path_name);
    if (result == AddProjectPathResult::ProjectNotFound) {
      std::cerr << "project not found\n";
      return 1;
    }
    if (result == AddProjectPathResult::PathAlreadyExistsWithDifferentName) {
      std::cerr << "path already exists with different name\n";
      return 1;
    }
    if (result == AddProjectPathResult::NameAlreadyExistsWithDifferentPath) {
      std::cerr << "name already exists with different path\n";
      return 1;
    }

    const fs::path link_dir = project_dir_path(project_name);
    const fs::path link_path = link_dir / path_name;
    if (result == AddProjectPathResult::AlreadyExists) {
      std::cout << "path already exists: " << normalized_path << '\n';
      return 0;
    }

    bool rollback_required = true;
    try {
      fs::create_directories(link_dir);
      const fs::file_status link_status = fs::symlink_status(link_path);
      if (link_status.type() != fs::file_type::not_found) {
        if (!is_same_symlink_target(link_path, absolute_path)) {
          throw std::runtime_error(
              "link already exists with different target: " + link_path.string());
        }
      } else {
        fs::create_symlink(absolute_path, link_path);
      }
      rollback_required = false;
    } catch (const std::exception&) {
      if (rollback_required) {
        try {
          remove_project_path(projects_file, project_name, normalized_path);
        } catch (const std::exception&) {
        }
      }
      throw;
    }

    ProjectConfig project_config = load_required_project_config(project_config_file_path(project_name));
    project_config.paths.clear();
    for (const ProjectPathEntry& entry : load_project_path_entries(projects_file, project_name)) {
      project_config.paths.push_back(entry.name);
    }
    save_project_config(project_config_file_path(project_name), project_config);

    std::cout << "added path: " << normalized_path << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap add failed: " << e.what() << '\n';
    return 1;
  }
}
