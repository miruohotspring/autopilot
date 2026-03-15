#include "autopilot/projects/todo_task_selector.hpp"

#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const std::regex kTodoLineRe(R"(^- \[( |x)\] \[([a-z][a-z0-9-]*-[0-9]{4})\] (.+)$)");
const std::regex kTodoCheckboxPrefixRe(R"(^- \[( |x)\]\s+)");
const std::regex kTodoIdPrefixRe(R"(^- \[( |x)\]\s+\[([^\]]*)\](.*)$)");
const std::regex kTaskIdFormatRe(R"(^([a-z][a-z0-9-]*)-([0-9]{4})$)");

std::string trim_ascii_whitespace(const std::string& s) {
  const std::size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::vector<std::string> read_all_lines(const fs::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to open file: " + path.string());
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  return lines;
}

void write_all_lines(const fs::path& path, const std::vector<std::string>& lines) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open file for write: " + path.string());
  }

  for (const std::string& line : lines) {
    out << line << '\n';
  }
}

} // namespace

std::vector<TodoTaskSelection> list_todo_tasks(
    const fs::path& todo_file, const std::string& project_slug) {
  const std::vector<std::string> lines = read_all_lines(todo_file);
  std::vector<TodoTaskSelection> tasks;
  std::map<std::string, std::size_t> seen_task_ids;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].rfind("- [", 0) != 0) {
      continue;
    }

    std::smatch match;
    if (!std::regex_match(lines[i], match, kTodoLineRe)) {
      if (!std::regex_search(lines[i], kTodoCheckboxPrefixRe)) {
        throw std::runtime_error(
            "failed to parse TODO.md: invalid checkbox at line " + std::to_string(i + 1));
      }

      std::smatch id_match;
      if (!std::regex_match(lines[i], id_match, kTodoIdPrefixRe)) {
        throw std::runtime_error(
            "failed to parse TODO.md: task id is required at line " + std::to_string(i + 1));
      }

      const std::string invalid_task_id = id_match[2].str();
      std::smatch task_id_match;
      if (!std::regex_match(invalid_task_id, task_id_match, kTaskIdFormatRe)) {
        throw std::runtime_error(
            "failed to parse TODO.md: invalid task id format [" + invalid_task_id +
            "] at line " + std::to_string(i + 1));
      }

      if (task_id_match[1].str() != project_slug) {
        throw std::runtime_error(
            "failed to parse TODO.md: task id " + invalid_task_id +
            " does not match project slug " + project_slug + " at line " + std::to_string(i + 1));
      }

      if (trim_ascii_whitespace(id_match[3].str()).empty()) {
        throw std::runtime_error(
            "failed to parse TODO.md: empty task title at line " + std::to_string(i + 1));
      }
      throw std::runtime_error(
          "failed to parse TODO.md: invalid task format at line " + std::to_string(i + 1));
    }

    const std::string task_id = match[2].str();
    std::smatch task_id_match;
    if (!std::regex_match(task_id, task_id_match, kTaskIdFormatRe)) {
      throw std::runtime_error(
          "failed to parse TODO.md: invalid task id format [" + task_id +
          "] at line " + std::to_string(i + 1));
    }

    if (task_id_match[1].str() != project_slug) {
      throw std::runtime_error(
          "failed to parse TODO.md: task id " + task_id +
          " does not match project slug " + project_slug + " at line " + std::to_string(i + 1));
    }
    if (match[3].str().empty()) {
      throw std::runtime_error(
          "failed to parse TODO.md: empty task title at line " + std::to_string(i + 1));
    }
    if (seen_task_ids.find(task_id) != seen_task_ids.end()) {
      throw std::runtime_error(
          "failed to parse TODO.md: duplicate task id " + task_id + " at line " +
          std::to_string(i + 1));
    }
    seen_task_ids[task_id] = i + 1;
    tasks.push_back(TodoTaskSelection{
        task_id,
        match[3].str(),
        i + 1,
        lines[i],
        match[1].str() == "x",
    });
  }
  return tasks;
}

std::optional<TodoTaskSelection> select_first_todo_task(
    const fs::path& todo_file, const std::string& project_slug) {
  const std::vector<TodoTaskSelection> tasks = list_todo_tasks(todo_file, project_slug);
  for (const TodoTaskSelection& task : tasks) {
    if (!task.completed) {
      return task;
    }
  }
  return std::nullopt;
}

bool mark_todo_task_done(const fs::path& todo_file, const TodoTaskSelection& selection) {
  std::vector<std::string> lines = read_all_lines(todo_file);
  if (selection.source_line == 0 || selection.source_line > lines.size()) {
    return false;
  }

  std::string& target_line = lines[selection.source_line - 1];
  if (target_line != selection.original_line_text) {
    return false;
  }
  if (target_line.rfind("- [ ] ", 0) != 0) {
    return false;
  }

  target_line.replace(3, 1, "x");
  write_all_lines(todo_file, lines);
  return true;
}
