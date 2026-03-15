#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct TodoTaskSelection {
  std::string title;
  std::size_t source_line;
  std::string original_line_text;
  bool completed;
};

std::vector<TodoTaskSelection> list_todo_tasks(const std::filesystem::path& todo_file);
std::optional<TodoTaskSelection> select_first_todo_task(const std::filesystem::path& todo_file);
bool mark_todo_task_done(
    const std::filesystem::path& todo_file, const TodoTaskSelection& selection);
