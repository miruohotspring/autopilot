#include "autopilot/projects/project_store.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const std::regex kTopLevelKeyRe(R"(^([A-Za-z0-9-]+)\s*:)");

}  // namespace

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

  std::string line;
  std::smatch match;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == ' ' || line[0] == '\t' || line[0] == '#') {
      continue;
    }
    if (std::regex_search(line, match, kTopLevelKeyRe)) {
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

bool remove_project_block(const fs::path& projects_file, const std::string& project_name) {
  std::ifstream in(projects_file);
  if (!in) {
    throw std::runtime_error("failed to open projects file: " + projects_file.string());
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  in.close();

  std::vector<std::string> kept;
  kept.reserve(lines.size());

  bool found = false;
  bool dropping_block = false;
  for (const std::string& current_line : lines) {
    if (dropping_block) {
      std::smatch match;
      if (std::regex_search(current_line, match, kTopLevelKeyRe)) {
        dropping_block = false;
      } else if (!current_line.empty() && current_line[0] != ' ' && current_line[0] != '\t') {
        dropping_block = false;
      } else {
        continue;
      }
    }

    std::smatch match;
    if (std::regex_search(current_line, match, kTopLevelKeyRe) && match[1].str() == project_name) {
      found = true;
      dropping_block = true;
      continue;
    }

    kept.push_back(current_line);
  }

  if (!found) {
    return false;
  }

  std::ofstream out(projects_file, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open projects file for write: " + projects_file.string());
  }

  for (const std::string& kept_line : kept) {
    out << kept_line << '\n';
  }

  return true;
}
