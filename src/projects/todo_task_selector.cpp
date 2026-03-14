#include "autopilot/projects/todo_task_selector.hpp"

#include <fstream>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const std::regex kTodoLineRe(R"(^- \[ \] (.+)$)");

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

std::optional<TodoTaskSelection> select_first_todo_task(const fs::path& todo_file) {
  const std::vector<std::string> lines = read_all_lines(todo_file);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    std::smatch match;
    if (std::regex_match(lines[i], match, kTodoLineRe)) {
      return TodoTaskSelection{match[1].str(), i + 1, lines[i]};
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
