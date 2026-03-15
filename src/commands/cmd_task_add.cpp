#include "autopilot/commands/cmd_task_add.hpp"

#include "autopilot/commands/delete_ui.hpp"
#include "autopilot/projects/project_paths.hpp"
#include "autopilot/projects/project_store.hpp"
#include "autopilot/projects/todo_task_selector.hpp"
#include "autopilot/runtime/task_state_store.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string current_timestamp_with_offset() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_tm{};
#if defined(_WIN32)
  localtime_s(&local_tm, &now_time);
#else
  localtime_r(&now_time, &local_tm);
#endif

  std::ostringstream oss;
  oss << std::put_time(&local_tm, "%Y-%m-%dT%H:%M:%S%z");
  std::string timestamp = oss.str();
  if (timestamp.size() >= 5) {
    timestamp.insert(timestamp.size() - 2, ":");
  }
  return timestamp;
}

std::string trim_ascii_whitespace(const std::string& s) {
  const std::size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::vector<std::string> initial_related_paths(const std::vector<ProjectPathEntry>& entries) {
  for (const ProjectPathEntry& entry : entries) {
    if (entry.name == "main") {
      return {entry.name};
    }
  }
  return {entries.front().name};
}

std::vector<std::string> read_all_lines(const fs::path& file) {
  std::ifstream in(file);
  if (!in) {
    throw std::runtime_error("failed to open file: " + file.string());
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
    throw std::runtime_error("failed to open file for write: " + file.string());
  }

  for (const std::string& line : lines) {
    out << line << '\n';
  }
}

fs::path todo_temp_file_path(const fs::path& todo_file) {
  return todo_file.parent_path() / (todo_file.filename().string() + ".ap-task-add.tmp");
}

fs::path staged_tasks_dir_path(const fs::path& tasks_dir) {
  return tasks_dir / ".ap-task-add-staging";
}

void write_all_lines_atomic(const fs::path& file, const std::vector<std::string>& lines) {
  const fs::path temp_file = todo_temp_file_path(file);
  write_all_lines(temp_file, lines);
  fs::rename(temp_file, file);
}

std::vector<std::string> build_appended_todo_lines(
    const std::vector<std::string>& original_lines, const std::string& task_line) {
  std::vector<std::string> lines = original_lines;
  if (!lines.empty() && !lines.back().empty()) {
    lines.push_back("");
  }
  lines.push_back(task_line);
  return lines;
}

void cleanup_staged_task_file(const fs::path& staged_task_file) {
  std::error_code ec;
  fs::remove(staged_task_file, ec);
  fs::remove(staged_task_file.parent_path(), ec);
}

void validate_todo_state_consistency(
    const std::vector<TodoTaskSelection>& todo_tasks, const std::vector<TaskState>& tasks) {
  std::set<std::string> known_task_ids;
  for (const TaskState& task : tasks) {
    known_task_ids.insert(task.id);
  }
  for (const TodoTaskSelection& todo_task : todo_tasks) {
    if (known_task_ids.find(todo_task.task_id) == known_task_ids.end()) {
      throw std::runtime_error(
          "failed to sync TODO.md: task id " + todo_task.task_id + " does not exist in state");
    }
  }
}

} // namespace

int cmd_task_add(
    const std::string& title, const std::optional<std::string>& maybe_project_name) {
  const std::string trimmed_title = trim_ascii_whitespace(title);
  if (trimmed_title.empty()) {
    std::cerr << "task title is required\n";
    return 1;
  }
  if (maybe_project_name.has_value() && !is_valid_project_name(*maybe_project_name)) {
    std::cerr << "invalid project name\n";
    return 1;
  }

  if (!autopilot_dir_exists()) {
    std::cerr << "Please run ap init first\n";
    return 1;
  }

  const fs::path projects_file = projects_file_path();
  try {
    if (!fs::exists(projects_file)) {
      std::cerr << "project not found\n";
      return 1;
    }

    const std::set<std::string> existing_projects = load_top_level_projects(projects_file);
    if (existing_projects.empty()) {
      std::cerr << "project not found\n";
      return 1;
    }

    std::string project_name;
    if (maybe_project_name.has_value()) {
      project_name = *maybe_project_name;
      if (existing_projects.find(project_name) == existing_projects.end()) {
        std::cerr << "project not found\n";
        return 1;
      }
    } else {
      const std::optional<std::string> selected_project =
          select_project_to_add_task(existing_projects);
      if (!selected_project.has_value()) {
        std::cerr << "failed to read project selection\n";
        return 1;
      }
      project_name = *selected_project;
    }

    const ProjectConfig project_config =
        load_required_project_config(project_config_file_path(project_name));
    const fs::path project_dir = project_dir_path(project_name);
    const fs::path todo_file = project_dir / "TODO.md";
    if (!fs::exists(todo_file)) {
      throw std::runtime_error("TODO.md not found");
    }

    const std::vector<ProjectPathEntry> path_entries =
        load_project_path_entries(projects_file, project_name);
    if (path_entries.empty()) {
      throw std::runtime_error("no managed path");
    }
    const fs::path tasks_dir = project_dir / "runtime" / "state" / "tasks";
    const std::vector<std::string> original_todo_lines = read_all_lines(todo_file);
    const std::vector<TodoTaskSelection> todo_tasks = list_todo_tasks(todo_file, project_config.slug);
    std::vector<TaskState> tasks = load_task_states(tasks_dir);
    validate_todo_state_consistency(todo_tasks, tasks);

    TaskState task;
    task.id = allocate_next_task_id(tasks, project_config.slug);
    task.title = trimmed_title;
    task.description = std::nullopt;
    task.status = "todo";
    task.priority = 100;
    task.depends_on = {};
    task.approval_required = false;
    task.related_paths = initial_related_paths(path_entries);
    task.generated_by = "ap.task_add";
    task.source_file = "TODO.md";
    const std::string todo_line = "- [ ] [" + task.id + "] " + task.title;
    const std::vector<std::string> updated_todo_lines =
        build_appended_todo_lines(original_todo_lines, todo_line);
    task.source_line = updated_todo_lines.size();
    task.source_text = todo_line;
    task.present_in_todo = true;
    task.attempt_count = 0;
    task.latest_run_id = std::nullopt;
    task.last_error = std::nullopt;
    task.blocker_reason = std::nullopt;
    task.blocker_category = std::nullopt;
    task.created_at = current_timestamp_with_offset();
    task.updated_at = task.created_at;

    const fs::path staged_tasks_dir = staged_tasks_dir_path(tasks_dir);
    save_task_state(staged_tasks_dir, task);
    const fs::path staged_task_file = staged_tasks_dir / (task.id + ".json");
    const fs::path final_task_file = tasks_dir / (task.id + ".json");

    try {
      write_all_lines_atomic(todo_file, updated_todo_lines);
    } catch (const std::exception&) {
      cleanup_staged_task_file(staged_task_file);
      throw;
    }

    try {
      fs::create_directories(tasks_dir);
      fs::rename(staged_task_file, final_task_file);
      std::error_code ec;
      fs::remove(staged_tasks_dir, ec);
    } catch (const std::exception& e) {
      cleanup_staged_task_file(staged_task_file);
      try {
        write_all_lines_atomic(todo_file, original_todo_lines);
      } catch (const std::exception& rollback_error) {
        throw std::runtime_error(
            "failed to finalize ap task add transaction: state commit error: " +
            std::string(e.what()) + "; TODO rollback error: " + rollback_error.what());
      }
      throw std::runtime_error("failed to finalize ap task add transaction: " + std::string(e.what()));
    }

    std::cout << "added task: [" << task.id << "] " << task.title << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap task add failed: " << e.what() << '\n';
    return 1;
  }
}
