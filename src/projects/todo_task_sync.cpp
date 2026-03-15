#include "autopilot/projects/todo_task_sync.hpp"

#include "autopilot/projects/todo_task_selector.hpp"
#include "autopilot/runtime/json_utils.hpp"
#include "autopilot/runtime/task_state_store.hpp"

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const char* kDuplicateTodoTitleError =
    "duplicate TODO task titles are not supported in Phase 4";

std::optional<std::size_t> find_exact_todo_match(
    const std::vector<TaskState>& tasks,
    const std::vector<bool>& matched,
    const TodoTaskSelection& todo) {
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    const TaskState& task = tasks[i];
    if (matched[i]) {
      continue;
    }
    if (task.source_file == "TODO.md" && task.source_line == todo.source_line &&
        task.title == todo.title) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> find_unique_title_match(
    const std::vector<TaskState>& tasks,
    const std::vector<bool>& matched,
    const TodoTaskSelection& todo) {
  std::vector<std::size_t> title_matches;
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    if (matched[i]) {
      continue;
    }
    if (tasks[i].title == todo.title) {
      title_matches.push_back(i);
    }
  }

  if (title_matches.size() == 1) {
    return title_matches.front();
  }
  return std::nullopt;
}

std::optional<std::size_t> find_unique_source_line_match(
    const std::vector<TaskState>& tasks,
    const std::vector<bool>& matched,
    const TodoTaskSelection& todo) {
  std::vector<std::size_t> source_line_matches;
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    const TaskState& task = tasks[i];
    if (matched[i]) {
      continue;
    }
    if (task.source_file == "TODO.md" && task.source_line == todo.source_line) {
      source_line_matches.push_back(i);
    }
  }

  if (source_line_matches.size() == 1) {
    return source_line_matches.front();
  }
  return std::nullopt;
}

std::optional<TaskUpdateChange> apply_todo_entry_to_task(
    TaskState& task,
    const TodoTaskSelection& todo,
    const std::string& synced_at,
    bool& status_changed) {
  bool changed = false;
  TaskUpdateChange update{task.id, {}};
  if (task.title != todo.title) {
    update.fields.push_back(TaskFieldChange{
        "title",
        json_string(task.title),
        json_string(todo.title),
    });
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
  status_changed = old_status != task.status;
  if (update.fields.empty()) {
    return std::nullopt;
  }
  return update;
}

} // namespace

TodoSyncResult sync_todo_with_task_state(
    const std::filesystem::path& todo_file,
    const std::vector<TaskState>& existing_tasks,
    const std::string& synced_at,
    const std::string& default_related_path) {
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
    std::optional<std::size_t> selected_index =
        find_exact_todo_match(result.tasks, matched, todo);
    if (!selected_index.has_value()) {
      selected_index = find_unique_title_match(result.tasks, matched, todo);
    }
    if (!selected_index.has_value()) {
      selected_index = find_unique_source_line_match(result.tasks, matched, todo);
    }

    if (!selected_index.has_value()) {
      TaskState task;
      task.id = allocate_next_task_id(result.tasks);
      task.title = todo.title;
      task.description = std::nullopt;
      task.status = todo.completed ? "done" : "todo";
      task.priority = 100;
      task.depends_on = {};
      task.approval_required = false;
      task.related_paths = {default_related_path};
      task.generated_by = "human.todo";
      task.source_file = "TODO.md";
      task.source_line = todo.source_line;
      task.source_text = todo.original_line_text;
      task.present_in_todo = true;
      task.attempt_count = 0;
      task.latest_run_id = std::nullopt;
      task.last_error = std::nullopt;
      task.blocker_reason = std::nullopt;
      task.blocker_category = std::nullopt;
      task.created_at = synced_at;
      task.updated_at = synced_at;
      result.discovered_tasks.push_back(task);
      result.tasks.push_back(task);
      matched.push_back(true);
      continue;
    }

    TaskState& task = result.tasks[*selected_index];
    const std::string old_status = task.status;
    bool status_changed = false;
    const std::optional<TaskUpdateChange> update =
        apply_todo_entry_to_task(task, todo, synced_at, status_changed);
    matched[*selected_index] = true;
    if (status_changed) {
      result.status_changes.push_back(TaskStatusChange{task.id, old_status, task.status});
    }
    if (update.has_value()) {
      result.task_updates.push_back(*update);
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
