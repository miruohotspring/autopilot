#include "autopilot/commands/template_sync.hpp"

#include <array>
#include <filesystem>
#include <ostream>

namespace fs = std::filesystem;

namespace {

constexpr std::array<const char*, 5> kManagedRelativePaths = {{
    "CLAUDE.md",
    "skills/ap-self-recognition/SKILL.md",
    "skills/ap-self-recognition/agents/openai.yaml",
    "skills/ap-briefing/SKILL.md",
    "skills/ap-briefing/agents/openai.yaml",
}};

constexpr std::array<const char*, 2> kManagedSkillNames = {{
    "ap-self-recognition",
    "ap-briefing",
}};

bool is_same_symlink_target(const fs::path& link_path, const fs::path& expected_target) {
  if (!fs::is_symlink(link_path)) {
    return false;
  }
  return fs::read_symlink(link_path) == expected_target;
}

void ensure_real_directory(const fs::path& path) {
  const fs::file_status status = fs::symlink_status(path);
  if (status.type() == fs::file_type::not_found) {
    fs::create_directories(path);
    return;
  }
  if (status.type() == fs::file_type::symlink) {
    fs::remove(path);
    fs::create_directories(path);
    return;
  }
  if (!fs::is_directory(status)) {
    throw fs::filesystem_error(
        "managed path is not a directory",
        path,
        std::make_error_code(std::errc::not_a_directory));
  }
}

void ensure_real_parent_directories(const fs::path& root, const fs::path& path) {
  fs::path current = root;
  ensure_real_directory(current);

  const fs::path relative = path.lexically_relative(root);
  if (relative.empty() || relative == ".") {
    return;
  }

  for (const fs::path& part : relative) {
    current /= part;
    ensure_real_directory(current);
  }
}

void ensure_home_managed_skill_links(
    const fs::path& home_dir,
    const fs::path& dot_dir_name,
    std::ostream& out) {
  const fs::path dot_dir = home_dir / dot_dir_name;
  if (!fs::exists(dot_dir) || !fs::is_directory(dot_dir)) {
    return;
  }

  const fs::path skills_dir = dot_dir / "skills";
  fs::create_directories(skills_dir);

  for (const char* skill_name : kManagedSkillNames) {
    const fs::path link_path = skills_dir / skill_name;
    const fs::path expected_target = fs::path("..") / ".." / ".autopilot" / "skills" / skill_name;

    if (fs::exists(link_path) || fs::is_symlink(link_path)) {
      if (is_same_symlink_target(link_path, expected_target)) {
        continue;
      }
      fs::remove_all(link_path);
    }

    fs::create_directory_symlink(expected_target, link_path);
    out << "updated: " << link_path << '\n';
  }
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

void sync_managed_templates(
    const fs::path& template_root,
    const fs::path& home_dir,
    const fs::path& autopilot_dir,
    std::ostream& out) {
  for (const char* rel : managed_template_relative_paths()) {
    const fs::path source = template_root / rel;
    const fs::path destination = autopilot_dir / rel;
    ensure_real_parent_directories(autopilot_dir, destination.parent_path());
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing);
    out << "updated: " << destination << '\n';
  }

  ensure_home_managed_skill_links(home_dir, ".codex", out);
  ensure_home_managed_skill_links(home_dir, ".claude", out);
}
