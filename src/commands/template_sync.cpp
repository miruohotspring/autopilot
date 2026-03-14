#include "autopilot/commands/template_sync.hpp"

#include <array>
#include <filesystem>
#include <ostream>
#include <string_view>

namespace fs = std::filesystem;

namespace {

constexpr std::array<const char*, 5> kManagedRelativePaths = {{
    "CLAUDE.md",
    ".claude/skills/ap-self-recognition/SKILL.md",
    ".claude/skills/ap-self-recognition/agents/openai.yaml",
    ".claude/skills/ap-briefing/SKILL.md",
    ".claude/skills/ap-briefing/agents/openai.yaml",
}};

constexpr std::string_view kTemplateSkillsPrefix = ".claude/skills/";
constexpr std::string_view kManagedSkillsPrefix = "skills/";

fs::path managed_destination_relative_path(const char* rel) {
  std::string_view rel_view(rel);
  if (rel_view.rfind(kTemplateSkillsPrefix, 0) == 0) {
    return fs::path(kManagedSkillsPrefix) / rel_view.substr(kTemplateSkillsPrefix.size());
  }
  return fs::path(rel);
}

bool is_same_symlink_target(const fs::path& link_path, const fs::path& expected_target) {
  if (!fs::is_symlink(link_path)) {
    return false;
  }
  return fs::read_symlink(link_path) == expected_target;
}

void ensure_claude_skills_symlink(const fs::path& autopilot_dir) {
  const fs::path claude_dir = autopilot_dir / ".claude";
  const fs::path link_path = claude_dir / "skills";
  const fs::path expected_target = "../skills";

  fs::create_directories(claude_dir);

  if (fs::exists(link_path) || fs::is_symlink(link_path)) {
    if (is_same_symlink_target(link_path, expected_target)) {
      return;
    }
    fs::remove_all(link_path);
  }

  fs::create_directory_symlink(expected_target, link_path);
}

} // namespace

fs::path managed_templates_root_from_cwd() {
  return fs::current_path() / "templates" / "autopilot";
}

const ManagedTemplateList& managed_template_relative_paths() {
  return kManagedRelativePaths;
}

std::optional<fs::path> first_missing_managed_template(const fs::path& template_root) {
  if (!fs::exists(template_root) || !fs::is_directory(template_root)) {
    return template_root;
  }

  for (const char* rel : managed_template_relative_paths()) {
    const fs::path source = template_root / rel;
    if (!fs::exists(source) || !fs::is_regular_file(source)) {
      return source;
    }
  }
  return std::nullopt;
}

void sync_managed_templates(const fs::path& template_root, const fs::path& autopilot_dir, std::ostream& out) {
  for (const char* rel : managed_template_relative_paths()) {
    const fs::path source = template_root / rel;
    const fs::path destination = autopilot_dir / managed_destination_relative_path(rel);
    fs::create_directories(destination.parent_path());
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing);
    out << "updated: " << destination << '\n';
  }

  ensure_claude_skills_symlink(autopilot_dir);
  out << "updated: " << (autopilot_dir / ".claude" / "skills") << '\n';
}
