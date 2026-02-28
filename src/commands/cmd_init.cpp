#include "autopilot/commands/cmd_init.hpp"
#include "autopilot/platform/home_dir.hpp"

#include <filesystem>
#include <exception>
#include <iostream>

namespace fs = std::filesystem;

int cmd_init() {
  const fs::path home = get_home_dir();
  const fs::path autopilot_dir = home / ".autopilot";
  const fs::path projects_dir = autopilot_dir / "projects";
  const fs::path backup_dir = home / ".autopilot.bak";

  try {
    if (fs::exists(autopilot_dir)) {
      if (fs::exists(backup_dir)) {
        std::cerr << "ap init failed: backup already exists: " << backup_dir << '\n';
        return 1;
      }
      fs::rename(autopilot_dir, backup_dir);
      std::cout << "renamed: " << autopilot_dir << " -> " << backup_dir << '\n';
    }

    fs::create_directories(projects_dir);
    std::cout << "created: " << autopilot_dir << '\n';
    std::cout << "created: " << projects_dir << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap init failed: " << e.what() << '\n';
    return 1;
  }
}
