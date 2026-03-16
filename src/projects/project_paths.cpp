#include "autopilot/projects/project_paths.hpp"
#include "autopilot/platform/home_dir.hpp"

#include <filesystem>

namespace fs = std::filesystem;

fs::path autopilot_dir_path() {
  return fs::path(get_home_dir()) / ".autopilot";
}

fs::path projects_dir_path() {
  return autopilot_dir_path() / "projects";
}

fs::path project_dir_path(const std::string& project_name) {
  return projects_dir_path() / project_name;
}

fs::path projects_file_path() {
  return autopilot_dir_path() / "projects.yaml";
}

bool autopilot_dir_exists() {
  const fs::path autopilot_dir = autopilot_dir_path();
  return fs::exists(autopilot_dir) && fs::is_directory(autopilot_dir);
}
