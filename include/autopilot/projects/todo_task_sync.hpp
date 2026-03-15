#pragma once

#include "autopilot/runtime/task_state_store.hpp"

#include <filesystem>
#include <string>
#include <vector>

struct TaskStatusChange {
  std::string task_id;
  std::string from_status;
  std::string to_status;
};

struct TaskFieldChange {
  std::string key;
  std::string previous_json;
  std::string current_json;
};

struct TaskUpdateChange {
  std::string task_id;
  std::vector<TaskFieldChange> fields;
};

struct TodoSyncResult {
  std::vector<TaskState> tasks;
  std::vector<TaskState> discovered_tasks;
  std::vector<TaskStatusChange> status_changes;
  std::vector<TaskUpdateChange> task_updates;
};

TodoSyncResult sync_todo_with_task_state(
    const std::filesystem::path& todo_file,
    const std::vector<TaskState>& existing_tasks,
    const std::string& synced_at,
    const std::string& default_related_path);
