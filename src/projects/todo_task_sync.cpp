#include "autopilot/projects/todo_task_sync.hpp"

#include "autopilot/projects/todo_task_selector.hpp"
#include "autopilot/runtime/task_state_store.hpp"

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const char* kDuplicateTodoTitleError =
    "duplicate TODO task titles are not supported in Phase 2";

bool apply_todo_entry_to_task(
    TaskState& task, const TodoTaskSelection& todo, const std::string& synced_at) {
  bool changed = false;
  if (task.title != todo.title) {
    task.title = todo.title;
    changed = true;
  }
  if (task.source_file != "TODO.md") {
    task.source_file = "TODO.md";
    changed = true;
  }
  if (task.source_line != todo.source_line) {
    task.source_line = todo.source_line;
    changed = true;
  }
  if (task.source_text != todo.original_line_text) {
    task.source_text = todo.original_line_text;
    changed = true;
  }
  if (!task.present_in_todo) {
    task.present_in_todo = true;
    changed = true;
  }

  const std::string old_status = task.status;
  if (todo.completed && task.status != "done") {
    task.status = "done";
    changed = true;
  } else if (!todo.completed && task.status == "done") {
    task.status = "todo";
    changed = true;
  }

  if (changed || old_status != task.status) {
    task.updated_at = synced_at;
  }
  return old_status != task.status;
}

} // namespace

TodoSyncResult sync_todo_with_task_state(
    const std::filesystem::path& todo_file,
    const std::vector<TaskState>& existing_tasks,
    const std::string& synced_at) {
  const std::vector<TodoTaskSelection> todo_tasks = list_todo_tasks(todo_file);

  std::map<std::string, int> open_title_counts;
  for (const TodoTaskSelection& todo : todo_tasks) {
    if (!todo.completed) {
      ++open_title_counts[todo.title];
      if (open_title_counts[todo.title] > 1) {
        throw std::runtime_error(kDuplicateTodoTitleError);
      }
    }
  }

  TodoSyncResult result;
  result.tasks = existing_tasks;
  std::vector<bool> matched(result.tasks.size(), false);

  for (const TodoTaskSelection& todo : todo_tasks) {
    std::optional<std::size_t> selected_index;

    for (std::size_t i = 0; i < result.tasks.size(); ++i) {
      const TaskState& task = result.tasks[i];
      if (matched[i]) {
        continue;
      }
      if (task.source_file == "TODO.md" && task.source_line == todo.source_line &&
          task.title == todo.title) {
        selected_index = i;
        break;
      }
    }

    if (!selected_index.has_value()) {
      std::vector<std::size_t> title_matches;
      for (std::size_t i = 0; i < result.tasks.size(); ++i) {
        if (matched[i]) {
          continue;
        }
        if (result.tasks[i].title == todo.title) {
          title_matches.push_back(i);
        }
      }

      if (title_matches.size() == 1) {
        selected_index = title_matches.front();
      }
    }

    if (!selected_index.has_value()) {
      TaskState task;
      task.id = allocate_next_task_id(result.tasks);
      task.title = todo.title;
      task.status = todo.completed ? "done" : "todo";
      task.source_file = "TODO.md";
      task.source_line = todo.source_line;
      task.source_text = todo.original_line_text;
      task.present_in_todo = true;
      task.attempt_count = 0;
      task.latest_run_id = std::nullopt;
      task.last_error = std::nullopt;
      task.created_at = synced_at;
      task.updated_at = synced_at;
      result.discovered_tasks.push_back(task);
      result.tasks.push_back(task);
      matched.push_back(true);
      continue;
    }

    TaskState& task = result.tasks[*selected_index];
    const std::string old_status = task.status;
    const bool status_changed = apply_todo_entry_to_task(task, todo, synced_at);
    matched[*selected_index] = true;
    if (status_changed) {
      result.status_changes.push_back(TaskStatusChange{task.id, old_status, task.status});
    }
  }

  for (std::size_t i = 0; i < result.tasks.size(); ++i) {
    if (matched[i]) {
      continue;
    }
    TaskState& task = result.tasks[i];
    if (task.present_in_todo) {
      task.present_in_todo = false;
      task.updated_at = synced_at;
    }
  }

  return result;
}
