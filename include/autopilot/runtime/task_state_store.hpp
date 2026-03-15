#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct TaskState {
  std::string id;
  std::string title;
  std::optional<std::string> description;
  std::string status;
  int priority;
  std::vector<std::string> depends_on;
  bool approval_required;
  std::vector<std::string> related_paths;
  std::string generated_by;
  std::string source_file;
  std::size_t source_line;
  std::string source_text;
  bool present_in_todo;
  int attempt_count;
  std::optional<std::string> latest_run_id;
  std::optional<std::string> last_error;
  std::optional<std::string> blocker_reason;
  std::optional<std::string> blocker_category;
  std::string created_at;
  std::string updated_at;
};

struct ProjectTaskCounts {
  int todo = 0;
  int in_progress = 0;
  int review_pending = 0;
  int blocked = 0;
  int done = 0;
  int failed = 0;
  int cancelled = 0;
};

struct ProjectState {
  std::string project;
  std::string status;
  std::optional<std::string> active_task_id;
  std::optional<std::string> last_run_id;
  std::optional<std::string> last_run_at;
  ProjectTaskCounts task_counts;
  std::string updated_at;
};

std::vector<TaskState> load_task_states(
    const std::filesystem::path& tasks_dir,
    const std::string& default_related_path = "main");
std::optional<ProjectState> load_project_state(const std::filesystem::path& project_file);

void save_task_state(const std::filesystem::path& tasks_dir, const TaskState& task);
void save_task_states(const std::filesystem::path& tasks_dir, const std::vector<TaskState>& tasks);
void save_project_state(const std::filesystem::path& project_file, const ProjectState& project);

std::string allocate_next_task_id(const std::vector<TaskState>& tasks, const std::string& project_slug);
ProjectTaskCounts compute_project_task_counts(const std::vector<TaskState>& tasks);
