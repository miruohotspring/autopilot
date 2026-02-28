#include "autopilot/commands/cmd_list.hpp"
#include "autopilot/projects/project_paths.hpp"
#include "autopilot/projects/project_store.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int cmd_list() {
  if (!autopilot_dir_exists()) {
    std::cerr << "Please run ap init first\n";
    return 1;
  }

  const fs::path projects_file = projects_file_path();
  try {
    if (!fs::exists(projects_file)) {
      std::cout << "no projects\n";
      return 0;
    }

    const auto projects = load_top_level_projects(projects_file);
    if (projects.empty()) {
      std::cout << "no projects\n";
      return 0;
    }

    for (const std::string& project_name : projects) {
      std::cout << project_name << '\n';
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap list failed: " << e.what() << '\n';
    return 1;
  }
}
