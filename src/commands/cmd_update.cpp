#include "autopilot/commands/cmd_update.hpp"
#include "autopilot/commands/template_sync.hpp"
#include "autopilot/platform/home_dir.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>

namespace fs = std::filesystem;

int cmd_update() {
  const fs::path autopilot_dir = fs::path(get_home_dir()) / ".autopilot";
  if (!fs::exists(autopilot_dir) || !fs::is_directory(autopilot_dir)) {
    std::cerr << "Please run ap init first\n";
    return 1;
  }

  const fs::path template_root = managed_templates_root_from_cwd();
  const std::optional<fs::path> maybe_missing = first_missing_managed_template(template_root);
  if (maybe_missing.has_value()) {
    if (*maybe_missing == template_root) {
      std::cerr << "ap update failed: run from autopilot repository root\n";
    } else {
      std::cerr << "ap update failed: missing template: " << *maybe_missing << '\n';
    }
    return 1;
  }

  try {
    sync_managed_templates(template_root, autopilot_dir, std::cout);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap update failed: " << e.what() << '\n';
    return 1;
  }
}
