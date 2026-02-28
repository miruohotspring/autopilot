#include "autopilot/commands/cmd_new.hpp"
#include "autopilot/platform/home_dir.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

bool is_valid_project_name(const std::string& name) {
  if (name.empty()) {
    return false;
  }
  if (name.front() == '-' || name.back() == '-') {
    return false;
  }

  for (const unsigned char c : name) {
    if (!(std::isalnum(c) || c == '-')) {
      return false;
    }
  }
  return true;
}

std::set<std::string> load_top_level_projects(const fs::path& projects_file) {
  std::set<std::string> projects;
  std::ifstream in(projects_file);
  if (!in) {
    throw std::runtime_error("failed to open projects file: " + projects_file.string());
  }

  const std::regex top_level_key_re(R"(^([A-Za-z0-9-]+)\s*:)");
  std::string line;
  std::smatch match;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == ' ' || line[0] == '\t' || line[0] == '#') {
      continue;
    }
    if (std::regex_search(line, match, top_level_key_re)) {
      projects.insert(match[1].str());
    }
  }

  return projects;
}

bool file_ends_with_newline(const fs::path& file) {
  if (!fs::exists(file) || fs::file_size(file) == 0) {
    return true;
  }

  std::ifstream in(file, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read file: " + file.string());
  }

  in.seekg(-1, std::ios::end);
  char ch = '\0';
  in.get(ch);
  return ch == '\n';
}

}  // namespace

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

  const fs::path home = get_home_dir();
  const fs::path autopilot_dir = home / ".autopilot";
  if (!fs::exists(autopilot_dir) || !fs::is_directory(autopilot_dir)) {
    std::cerr << "Please run ap init first\n";
    return 1;
  }

  const fs::path projects_file = autopilot_dir / "projects.yaml";

  try {
    if (!fs::exists(projects_file)) {
      std::ofstream create(projects_file);
      if (!create) {
        throw std::runtime_error("failed to create projects file: " + projects_file.string());
      }
    }

    const std::set<std::string> existing_projects = load_top_level_projects(projects_file);
    if (existing_projects.find(project_name) != existing_projects.end()) {
      std::cerr << "project already exists\n";
      return 1;
    }

    const bool needs_leading_newline = !file_ends_with_newline(projects_file);
    std::ofstream out(projects_file, std::ios::app);
    if (!out) {
      throw std::runtime_error("failed to open projects file for append: " + projects_file.string());
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
