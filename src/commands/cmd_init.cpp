#include "autopilot/commands/cmd_init.hpp"
#include "autopilot/commands/template_sync.hpp"
#include "autopilot/platform/home_dir.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>

namespace fs = std::filesystem;

int cmd_init() {
  const fs::path home = get_home_dir();
  const fs::path autopilot_dir = home / ".autopilot";
  const fs::path projects_dir = autopilot_dir / "projects";
  const fs::path backup_dir = home / ".autopilot.bak";
  const fs::path template_root = managed_templates_root_from_cwd();

  const std::optional<fs::path> maybe_missing = first_missing_managed_template(template_root);
  if (maybe_missing.has_value()) {
    if (*maybe_missing == template_root) {
      std::cerr << "ap init failed: run from autopilot repository root\n";
    } else {
      std::cerr << "ap init failed: missing template: " << *maybe_missing << '\n';
    }
    return 1;
  }

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
    sync_managed_templates(template_root, home, autopilot_dir, std::cout);

    std::cout << "created: " << autopilot_dir << '\n';
    std::cout << "created: " << projects_dir << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap init failed: " << e.what() << '\n';
    return 1;
  }
}
