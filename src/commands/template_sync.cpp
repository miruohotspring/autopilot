#include "autopilot/commands/template_sync.hpp"

#include <array>
#include <filesystem>
#include <ostream>

namespace fs = std::filesystem;

namespace {

constexpr std::array<const char*, 5> kManagedRelativePaths = {{
    "CLAUDE.md",
    ".claude/skills/self-recognition/SKILL.md",
    ".claude/skills/self-recognition/agents/openai.yaml",
    ".claude/skills/briefing/SKILL.md",
    ".claude/skills/briefing/agents/openai.yaml",
}};

} // namespace

fs::path managed_templates_root_from_cwd() {
  return fs::current_path() / "templates" / "autopilot";
}

std::optional<fs::path> first_missing_managed_template(const fs::path& template_root) {
  if (!fs::exists(template_root) || !fs::is_directory(template_root)) {
    return template_root;
  }

  for (const char* rel : kManagedRelativePaths) {
    const fs::path source = template_root / rel;
    if (!fs::exists(source) || !fs::is_regular_file(source)) {
      return source;
    }
  }
  return std::nullopt;
}

void sync_managed_templates(const fs::path& template_root, const fs::path& autopilot_dir, std::ostream& out) {
  for (const char* rel : kManagedRelativePaths) {
    const fs::path source = template_root / rel;
    const fs::path destination = autopilot_dir / rel;
    fs::create_directories(destination.parent_path());
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing);
    out << "updated: " << destination << '\n';
  }
}
