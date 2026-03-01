#include "autopilot/commands/cmd_new.hpp"
#include "autopilot/projects/project_paths.hpp"
#include "autopilot/projects/project_store.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

int cmd_new(const std::optional<std::string>& maybe_project_name) {
  std::string project_name;
  if (maybe_project_name.has_value()) {
    project_name = *maybe_project_name;
  } else {
    std::cout << "Enter your new project name: ";
    std::cout.flush();
    if (!std::getline(std::cin, project_name)) {
      std::cerr << "failed to read project name\n";
      return 1;
    }
  }

  if (!is_valid_project_name(project_name)) {
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
      std::ofstream create(projects_file);
      if (!create) {
        throw std::runtime_error("failed to create projects file: " + projects_file.string());
      }
    }

    const auto existing_projects = load_top_level_projects(projects_file);
    if (existing_projects.find(project_name) != existing_projects.end()) {
      std::cerr << "project already exists\n";
      return 1;
    }

    const fs::path new_project_dir = project_dir_path(project_name);
    fs::create_directories(new_project_dir);

    auto write_heading_file = [&](const fs::path& path, const std::string& suffix) {
      std::ofstream out_file(path, std::ios::trunc);
      if (!out_file) {
        throw std::runtime_error("failed to create project file: " + path.string());
      }
      out_file << "# " << project_name << " " << suffix << '\n';
    };

    auto write_dashboard_file = [&](const fs::path& path) {
      std::ofstream out_file(path, std::ios::trunc);
      if (!out_file) {
        throw std::runtime_error("failed to create project file: " + path.string());
      }

      out_file << "# " << project_name << " Dashboard\n\n";
      out_file << "## Status\n";
      out_file << "- summary:\n";
      out_file << "- updated_at:\n\n";
      out_file << "## Progress\n";
      out_file << "-\n\n";
      out_file << "## Blockers\n";
      out_file << "-\n\n";
      out_file << "## Needs General Decision\n";
      out_file << "-\n\n";
      out_file << "## Next Actions\n";
      out_file << "-\n";
    };

    write_heading_file(new_project_dir / "TODO.md", "TODO");
    write_dashboard_file(new_project_dir / "dashboard.md");

    const bool needs_leading_newline = !file_ends_with_newline(projects_file);
    std::ofstream out(projects_file, std::ios::app);
    if (!out) {
      throw std::runtime_error(
          "failed to open projects file for append: " + projects_file.string());
    }
    if (needs_leading_newline) {
      out << '\n';
    }
    out << project_name << ":\n";
    out << "  priority: 1\n";
    out << "  paths: []\n";
    std::cout << "created project: " << project_name << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap new failed: " << e.what() << '\n';
    return 1;
  }
}
