#include "autopilot/commands/cmd_update.hpp"
#include "autopilot/platform/home_dir.hpp"

#include <array>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct SyncTarget {
  const char* relative_path;
};

constexpr std::array<SyncTarget, 5> kSyncTargets = {{
    {"CLAUDE.md"},
    {".claude/skills/self-recognition/SKILL.md"},
    {".claude/skills/self-recognition/agents/openai.yaml"},
    {".claude/skills/briefing/SKILL.md"},
    {".claude/skills/briefing/agents/openai.yaml"},
}};

} // namespace

int cmd_update() {
  const fs::path autopilot_dir = fs::path(get_home_dir()) / ".autopilot";
  if (!fs::exists(autopilot_dir) || !fs::is_directory(autopilot_dir)) {
    std::cerr << "Please run ap init first\n";
    return 1;
  }

  const fs::path template_root = fs::current_path() / "templates" / "autopilot";
  if (!fs::exists(template_root) || !fs::is_directory(template_root)) {
    std::cerr << "ap update failed: run from autopilot repository root\n";
    return 1;
  }

  try {
    for (const SyncTarget& target : kSyncTargets) {
      const fs::path source = template_root / target.relative_path;
      const fs::path destination = autopilot_dir / target.relative_path;
      if (!fs::exists(source) || !fs::is_regular_file(source)) {
        std::cerr << "ap update failed: missing template: " << source << '\n';
        return 1;
      }

      fs::create_directories(destination.parent_path());
      fs::copy_file(source, destination, fs::copy_options::overwrite_existing);
      std::cout << "updated: " << destination << '\n';
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap update failed: " << e.what() << '\n';
    return 1;
  }
}
