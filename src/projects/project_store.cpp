#include "autopilot/projects/project_store.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

const std::regex kTopLevelKeyRe(R"(^([A-Za-z0-9-]+)\s*:)");
const std::regex kPathsLineRe(R"(^([ \t]+)paths\s*:\s*(.*)$)");
const std::regex kListItemRe(R"(^([ \t]*)-\s*(.*)$)");
const std::regex kKeyValueRe(R"(^([A-Za-z0-9_-]+)\s*:\s*(.*)$)");

struct ProjectRange {
  std::size_t begin;
  std::size_t end;
};

struct PathsEntry {
  std::size_t line_index;
  std::string indent;
  std::string tail;
};

struct ParsedPathItem {
  std::size_t begin_line;
  std::size_t end_line;
  std::optional<std::string> name;
  std::optional<std::string> path;
};

std::string trim_ascii_whitespace(const std::string& s) {
  const std::size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::string yaml_single_quote(const std::string& s) {
  std::string out = "'";
  for (const char ch : s) {
    if (ch == '\'') {
      out += "''";
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('\'');
  return out;
}

std::string parse_yaml_scalar(std::string s) {
  s = trim_ascii_whitespace(s);
  if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
    s = s.substr(1, s.size() - 2);
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\'' && i + 1 < s.size() && s[i + 1] == '\'') {
        out.push_back('\'');
        ++i;
      } else {
        out.push_back(s[i]);
      }
    }
    return out;
  }

  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    s = s.substr(1, s.size() - 2);
    std::string out;
    out.reserve(s.size());
    bool escaped = false;
    for (const char ch : s) {
      if (!escaped && ch == '\\') {
        escaped = true;
        continue;
      }
      if (escaped) {
        if (ch == 'n') {
          out.push_back('\n');
        } else if (ch == 't') {
          out.push_back('\t');
        } else {
          out.push_back(ch);
        }
        escaped = false;
      } else {
        out.push_back(ch);
      }
    }
    if (escaped) {
      out.push_back('\\');
    }
    return out;
  }

  return s;
}

std::optional<std::pair<std::string, std::string>> parse_yaml_key_value(std::string s) {
  s = trim_ascii_whitespace(s);
  std::smatch match;
  if (!std::regex_match(s, match, kKeyValueRe)) {
    return std::nullopt;
  }
  return std::make_pair(match[1].str(), trim_ascii_whitespace(match[2].str()));
}

std::vector<std::string> read_all_lines(const fs::path& file) {
  std::ifstream in(file);
  if (!in) {
    throw std::runtime_error("failed to open projects file: " + file.string());
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  return lines;
}

void write_all_lines(const fs::path& file, const std::vector<std::string>& lines) {
  std::ofstream out(file, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open projects file for write: " + file.string());
  }

  for (const std::string& line : lines) {
    out << line << '\n';
  }
}

std::optional<ProjectRange> find_project_range(
    const std::vector<std::string>& lines,
    const std::string& project_name) {
  std::size_t begin = lines.size();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    std::smatch match;
    if (std::regex_search(lines[i], match, kTopLevelKeyRe) && match[1].str() == project_name) {
      begin = i;
      break;
    }
  }
  if (begin == lines.size()) {
    return std::nullopt;
  }

  std::size_t end = lines.size();
  for (std::size_t i = begin + 1; i < lines.size(); ++i) {
    if (std::regex_search(lines[i], kTopLevelKeyRe)) {
      end = i;
      break;
    }
  }

  return ProjectRange{begin, end};
}

std::optional<PathsEntry> find_paths_entry(
    const std::vector<std::string>& lines,
    const ProjectRange& range) {
  for (std::size_t i = range.begin + 1; i < range.end; ++i) {
    std::smatch match;
    if (std::regex_match(lines[i], match, kPathsLineRe)) {
      return PathsEntry{i, match[1].str(), trim_ascii_whitespace(match[2].str())};
    }
  }
  return std::nullopt;
}

std::size_t find_yaml_child_section_end(
    const std::vector<std::string>& lines,
    const std::size_t parent_line_index,
    const std::size_t parent_indent_width,
    const std::size_t project_end) {
  for (std::size_t i = parent_line_index + 1; i < project_end; ++i) {
    const std::size_t first_non_ws = lines[i].find_first_not_of(" \t");
    if (first_non_ws == std::string::npos) {
      continue;
    }
    if (first_non_ws <= parent_indent_width) {
      return i;
    }
  }
  return project_end;
}

std::vector<ParsedPathItem> collect_paths_items(
    const std::vector<std::string>& lines,
    const std::size_t paths_line_index,
    const std::size_t parent_indent_width,
    const std::size_t project_end) {
  const std::size_t section_end =
      find_yaml_child_section_end(lines, paths_line_index, parent_indent_width, project_end);

  std::vector<ParsedPathItem> items;
  for (std::size_t i = paths_line_index + 1; i < section_end;) {
    std::smatch item_match;
    if (!std::regex_match(lines[i], item_match, kListItemRe)) {
      ++i;
      continue;
    }

    const std::string item_indent = item_match[1].str();
    if (item_indent.size() <= parent_indent_width) {
      ++i;
      continue;
    }

    ParsedPathItem item;
    item.begin_line = i;
    item.end_line = i + 1;

    const std::string list_item_tail = trim_ascii_whitespace(item_match[2].str());
    const std::optional<std::pair<std::string, std::string>> inline_key_value =
        parse_yaml_key_value(list_item_tail);
    if (!inline_key_value.has_value()) {
      item.path = parse_yaml_scalar(list_item_tail);
      items.push_back(item);
      ++i;
      continue;
    }

    if (inline_key_value->first == "name") {
      item.name = parse_yaml_scalar(inline_key_value->second);
    } else if (inline_key_value->first == "path") {
      item.path = parse_yaml_scalar(inline_key_value->second);
    } else {
      throw std::runtime_error("unsupported paths item key: " + inline_key_value->first);
    }

    std::size_t j = i + 1;
    for (; j < section_end; ++j) {
      std::smatch next_item_match;
      if (std::regex_match(lines[j], next_item_match, kListItemRe) &&
          next_item_match[1].str().size() == item_indent.size()) {
        break;
      }

      const std::size_t first_non_ws = lines[j].find_first_not_of(" \t");
      if (first_non_ws == std::string::npos) {
        continue;
      }
      if (first_non_ws <= item_indent.size()) {
        break;
      }

      const std::optional<std::pair<std::string, std::string>> nested_key_value =
          parse_yaml_key_value(lines[j]);
      if (!nested_key_value.has_value()) {
        continue;
      }
      if (nested_key_value->first == "name") {
        item.name = parse_yaml_scalar(nested_key_value->second);
      } else if (nested_key_value->first == "path") {
        item.path = parse_yaml_scalar(nested_key_value->second);
      } else {
        throw std::runtime_error("unsupported paths item key: " + nested_key_value->first);
      }
    }

    if (!item.path.has_value()) {
      throw std::runtime_error("unsupported paths item format: missing path");
    }

    item.end_line = j;
    items.push_back(item);
    i = j;
  }
  return items;
}

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

std::vector<std::string> load_project_paths(
    const fs::path& projects_file,
    const std::string& project_name) {
  const std::vector<std::string> lines = read_all_lines(projects_file);
  const std::optional<ProjectRange> range = find_project_range(lines, project_name);
  if (!range.has_value()) {
    return {};
  }

  const std::optional<PathsEntry> paths_entry = find_paths_entry(lines, *range);
  if (!paths_entry.has_value()) {
    return {};
  }

  if (paths_entry->tail == "[]") {
    return {};
  }
  if (!paths_entry->tail.empty()) {
    throw std::runtime_error("unsupported paths format in project: " + project_name);
  }

  const auto items = collect_paths_items(
      lines,
      paths_entry->line_index,
      paths_entry->indent.size(),
      range->end);

  std::vector<std::string> paths;
  paths.reserve(items.size());
  for (const auto& item : items) {
    if (!item.path.has_value()) {
      throw std::runtime_error("unsupported paths item format: missing path");
    }
    paths.push_back(*item.path);
  }
  return paths;
}

bool project_has_path_name(
    const fs::path& projects_file,
    const std::string& project_name,
    const std::string& path_name) {
  const std::vector<std::string> lines = read_all_lines(projects_file);
  const std::optional<ProjectRange> range = find_project_range(lines, project_name);
  if (!range.has_value()) {
    return false;
  }

  const std::optional<PathsEntry> paths_entry = find_paths_entry(lines, *range);
  if (!paths_entry.has_value() || paths_entry->tail == "[]") {
    return false;
  }
  if (!paths_entry->tail.empty()) {
    throw std::runtime_error("unsupported paths format in project: " + project_name);
  }

  const auto items = collect_paths_items(
      lines,
      paths_entry->line_index,
      paths_entry->indent.size(),
      range->end);
  for (const auto& item : items) {
    if (item.name.has_value() && *item.name == path_name) {
      return true;
    }
  }
  return false;
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
  const std::vector<std::string> lines = read_all_lines(projects_file);

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

  write_all_lines(projects_file, kept);
  return true;
}

AddProjectPathResult add_project_path(
    const fs::path& projects_file,
    const std::string& project_name,
    const std::string& path_value,
    const std::string& path_name) {
  std::vector<std::string> lines = read_all_lines(projects_file);
  const std::optional<ProjectRange> range = find_project_range(lines, project_name);
  if (!range.has_value()) {
    return AddProjectPathResult::ProjectNotFound;
  }

  const std::optional<PathsEntry> paths_entry = find_paths_entry(lines, *range);
  if (!paths_entry.has_value()) {
    std::string paths_indent = "  ";
    for (std::size_t i = range->begin + 1; i < range->end; ++i) {
      const std::size_t first_non_ws = lines[i].find_first_not_of(" \t");
      if (first_non_ws != std::string::npos && first_non_ws > 0) {
        paths_indent = lines[i].substr(0, first_non_ws);
        break;
      }
    }

    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(range->end), paths_indent + "paths:");
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(range->end + 1),
                 paths_indent + "  - name: " + yaml_single_quote(path_name));
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(range->end + 2),
                 paths_indent + "    path: " + yaml_single_quote(path_value));
    write_all_lines(projects_file, lines);
    return AddProjectPathResult::Added;
  }

  if (paths_entry->tail == "[]") {
    lines[paths_entry->line_index] = paths_entry->indent + "paths:";
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(paths_entry->line_index + 1),
                 paths_entry->indent + "  - name: " + yaml_single_quote(path_name));
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(paths_entry->line_index + 2),
                 paths_entry->indent + "    path: " + yaml_single_quote(path_value));
    write_all_lines(projects_file, lines);
    return AddProjectPathResult::Added;
  }

  if (!paths_entry->tail.empty()) {
    throw std::runtime_error("unsupported paths format in project: " + project_name);
  }

  const auto items = collect_paths_items(
      lines,
      paths_entry->line_index,
      paths_entry->indent.size(),
      range->end);
  for (const auto& item : items) {
    if (!item.path.has_value()) {
      throw std::runtime_error("unsupported paths item format: missing path");
    }
    if (*item.path == path_value) {
      if (item.name.has_value() && *item.name == path_name) {
        return AddProjectPathResult::AlreadyExists;
      }
      return AddProjectPathResult::PathAlreadyExistsWithDifferentName;
    }
    if (item.name.has_value() && *item.name == path_name) {
      return AddProjectPathResult::NameAlreadyExistsWithDifferentPath;
    }
  }

  const std::size_t insert_index = find_yaml_child_section_end(
      lines,
      paths_entry->line_index,
      paths_entry->indent.size(),
      range->end);

  lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_index),
               paths_entry->indent + "  - name: " + yaml_single_quote(path_name));
  lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_index + 1),
               paths_entry->indent + "    path: " + yaml_single_quote(path_value));
  write_all_lines(projects_file, lines);
  return AddProjectPathResult::Added;
}

RemoveProjectPathResult remove_project_path(
    const fs::path& projects_file,
    const std::string& project_name,
    const std::string& path_value) {
  std::vector<std::string> lines = read_all_lines(projects_file);
  const std::optional<ProjectRange> range = find_project_range(lines, project_name);
  if (!range.has_value()) {
    return RemoveProjectPathResult::ProjectNotFound;
  }

  const std::optional<PathsEntry> paths_entry = find_paths_entry(lines, *range);
  if (!paths_entry.has_value()) {
    return RemoveProjectPathResult::PathNotFound;
  }

  if (paths_entry->tail == "[]") {
    return RemoveProjectPathResult::PathNotFound;
  }
  if (!paths_entry->tail.empty()) {
    throw std::runtime_error("unsupported paths format in project: " + project_name);
  }

  const auto items = collect_paths_items(
      lines,
      paths_entry->line_index,
      paths_entry->indent.size(),
      range->end);

  std::size_t remove_begin_index = lines.size();
  std::size_t remove_end_index = lines.size();
  for (const auto& item : items) {
    if (!item.path.has_value()) {
      throw std::runtime_error("unsupported paths item format: missing path");
    }
    if (*item.path == path_value) {
      remove_begin_index = item.begin_line;
      remove_end_index = item.end_line;
      break;
    }
  }
  if (remove_begin_index == lines.size()) {
    return RemoveProjectPathResult::PathNotFound;
  }

  const std::size_t removed_line_count = remove_end_index - remove_begin_index;
  lines.erase(
      lines.begin() + static_cast<std::ptrdiff_t>(remove_begin_index),
      lines.begin() + static_cast<std::ptrdiff_t>(remove_end_index));

  const std::size_t adjusted_project_end = range->end - removed_line_count;
  const auto remaining_items = collect_paths_items(
      lines,
      paths_entry->line_index,
      paths_entry->indent.size(),
      adjusted_project_end);

  if (remaining_items.empty()) {
    const std::size_t child_section_end = find_yaml_child_section_end(
        lines,
        paths_entry->line_index,
        paths_entry->indent.size(),
        adjusted_project_end);

    if (child_section_end > paths_entry->line_index + 1) {
      lines.erase(
          lines.begin() + static_cast<std::ptrdiff_t>(paths_entry->line_index + 1),
          lines.begin() + static_cast<std::ptrdiff_t>(child_section_end));
    }
    lines[paths_entry->line_index] = paths_entry->indent + "paths: []";
  }

  write_all_lines(projects_file, lines);
  return RemoveProjectPathResult::Removed;
}
