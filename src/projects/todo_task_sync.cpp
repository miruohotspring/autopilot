#include "autopilot/projects/todo_task_sync.hpp"

#include "autopilot/projects/todo_task_selector.hpp"
#include "autopilot/runtime/json_utils.hpp"
#include "autopilot/runtime/task_state_store.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

TaskUpdateChange apply_todo_entry_to_task(
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
  return update;
}

} // namespace

TodoSyncResult sync_todo_with_task_state(
    const std::filesystem::path& todo_file,
    const std::vector<TaskState>& existing_tasks,
    const std::string& synced_at,
    const std::string& default_related_path,
    const std::string& project_slug) {
  (void)default_related_path;
  const std::vector<TodoTaskSelection> todo_tasks = list_todo_tasks(todo_file, project_slug);

  TodoSyncResult result;
  result.tasks = existing_tasks;
  std::map<std::string, std::size_t> task_index;
  for (std::size_t i = 0; i < result.tasks.size(); ++i) {
    task_index[result.tasks[i].id] = i;
  }
  std::vector<bool> matched(result.tasks.size(), false);

  for (const TodoTaskSelection& todo : todo_tasks) {
    const auto selected_index = task_index.find(todo.task_id);
    if (selected_index == task_index.end()) {
      throw std::runtime_error(
          "failed to sync TODO.md: task id " + todo.task_id + " does not exist in state");
    }

    TaskState& task = result.tasks[selected_index->second];
    const std::string old_status = task.status;
    bool status_changed = false;
    TaskUpdateChange update = apply_todo_entry_to_task(task, todo, synced_at, status_changed);
    matched[selected_index->second] = true;
    if (status_changed) {
      result.status_changes.push_back(TaskStatusChange{task.id, old_status, task.status});
    }
    if (!update.fields.empty()) {
      result.task_updates.push_back(update);
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
