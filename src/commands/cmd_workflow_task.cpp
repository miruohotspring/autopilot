#include "autopilot/commands/cmd_workflow.hpp"

#include "autopilot/projects/todo_task_selector.hpp"
#include "autopilot/runtime/workflow.hpp"

#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

int cmd_task_sync(const std::optional<std::string>& maybe_project_name) {
  try {
    SyncedContext synced = load_synced_context(maybe_project_name, "ap.task.sync");
    EventLog event_log(synced.runtime.events_file);
    persist_synced_state(
        synced.runtime,
        event_log,
        synced.sync_result,
        {},
        synced.runtime.run_counter,
        synced.synced_at,
        "ap.task.sync");
    std::cout << "synced task state: " << synced.runtime.project_name << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap task sync failed: " << e.what() << '\n';
    return 1;
  }
}

int cmd_task_next(const std::optional<std::string>& maybe_project_name) {
  try {
    SyncedContext synced = load_synced_context(maybe_project_name, "ap.task.next");
    EventLog event_log(synced.runtime.events_file);
    const std::vector<TaskStatusChange> recovered =
        recover_stale_tasks(synced.sync_result.tasks, synced.runtime.runs_dir, synced.synced_at);
    persist_synced_state(
        synced.runtime,
        event_log,
        synced.sync_result,
        recovered,
        synced.runtime.run_counter,
        synced.synced_at,
        "ap.task.next");

    int retry_exhausted_count = 0;
    TaskState* selected_task = select_runnable_task(
        synced.sync_result.tasks, synced.runtime.selected_path.name, retry_exhausted_count);
    if (selected_task == nullptr) {
      std::cerr << "ap task next failed: no runnable task\n";
      return 1;
    }
    std::cout << selected_task->id << '\t' << selected_task->title << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap task next failed: " << e.what() << '\n';
    return 1;
  }
}

int cmd_task_set_status(
    const std::optional<std::string>& maybe_project_name,
    const std::string& task_id,
    const std::string& status) {
  try {
    const std::string normalized_status = normalize_task_status_override(status);
    SyncedContext synced = load_synced_context(maybe_project_name, "ap.task.set-status");
    EventLog event_log(synced.runtime.events_file);
    persist_synced_state(
        synced.runtime,
        event_log,
        synced.sync_result,
        {},
        synced.runtime.run_counter,
        synced.synced_at,
        "ap.task.set-status");

    TaskState* task = find_task_by_id(synced.sync_result.tasks, task_id);
    if (task == nullptr) {
      throw std::runtime_error("task not found");
    }
    const std::string previous_status = task->status;
    const std::string updated_at = current_timestamp_with_offset();
    task->status = normalized_status;
    task->updated_at = updated_at;
    if (normalized_status == "todo") {
      task->last_run_exit_reason = std::nullopt;
      task->last_error = std::nullopt;
      task->blocker_reason = std::nullopt;
      task->blocker_category = std::nullopt;
      task->review_result = std::nullopt;
    }
    if (normalized_status == "done") {
      const bool todo_updated = mark_todo_task_done(
          synced.runtime.todo_file,
          TodoTaskSelection{
              task->id,
              task->title,
              task->source_line,
              task->source_text,
              false,
          });
      if (todo_updated) {
        task->source_text = completed_todo_line(task->source_text);
      }
    }
    save_task_state(synced.runtime.tasks_dir, *task);
    const ProjectState project_state = build_project_state(
        synced.runtime.project_name,
        synced.sync_result.tasks,
        synced.runtime.previous_project_state,
        std::nullopt,
        std::nullopt,
        synced.runtime.previous_project_state.has_value()
            ? synced.runtime.previous_project_state->last_run_id
            : std::nullopt,
        synced.runtime.previous_project_state.has_value()
            ? synced.runtime.previous_project_state->last_run_at
            : std::nullopt,
        synced.runtime.run_counter,
        synced.runtime.default_timeout_seconds,
        updated_at);
    save_project_state(synced.runtime.project_state_file, project_state);
    append_status_changed_event(
        event_log,
        synced.runtime.project_name,
        *task,
        std::nullopt,
        previous_status,
        task->status,
        "manual_override",
        "ap.task.set-status");
    std::cout << "updated task status: " << task->id << " -> " << task->status << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap task set-status failed: " << e.what() << '\n';
    return 1;
  }
}

int cmd_recover(const std::optional<std::string>& maybe_project_name) {
  try {
    SyncedContext synced = load_synced_context(maybe_project_name, "ap.recover");
    EventLog event_log(synced.runtime.events_file);
    const std::vector<TaskStatusChange> recovered =
        recover_stale_tasks(synced.sync_result.tasks, synced.runtime.runs_dir, synced.synced_at);
    persist_synced_state(
        synced.runtime,
        event_log,
        synced.sync_result,
        recovered,
        synced.runtime.run_counter,
        synced.synced_at,
        "ap.recover");
    std::cout << "recovered tasks: " << recovered.size() << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap recover failed: " << e.what() << '\n';
    return 1;
  }
}
