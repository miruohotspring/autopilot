#include "autopilot/commands/cmd_start.hpp"

#include "autopilot/agents/agent_launcher.hpp"
#include "autopilot/commands/delete_ui.hpp"
#include "autopilot/projects/project_paths.hpp"
#include "autopilot/projects/project_store.hpp"
#include "autopilot/projects/todo_task_selector.hpp"
#include "autopilot/projects/todo_task_sync.hpp"
#include "autopilot/runtime/alert_store.hpp"
#include "autopilot/runtime/event_log.hpp"
#include "autopilot/runtime/json_utils.hpp"
#include "autopilot/runtime/run_result_classifier.hpp"
#include "autopilot/runtime/task_state_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct SelectedProjectPath {
  std::string name;
  fs::path path;
};

std::string shell_quote(const std::string& s) {
  std::string quoted = "'";
  for (const char ch : s) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

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

std::string current_run_id(const std::size_t source_line) {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_tm{};
#if defined(_WIN32)
  localtime_s(&local_tm, &now_time);
#else
  localtime_r(&now_time, &local_tm);
#endif

  std::ostringstream oss;
  oss << "run-" << std::put_time(&local_tm, "%Y%m%d-%H%M%S") << "-L" << source_line;
  return oss.str();
}

std::string allocate_run_id(const fs::path& runs_dir, const std::size_t source_line) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    const std::string run_id = current_run_id(source_line);
    if (!fs::exists(runs_dir / run_id)) {
      return run_id;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("failed to allocate unique run id");
}

std::string build_prompt(
    const std::string& project_name,
    const SelectedProjectPath& selected_path,
    const TaskState& task) {
  std::ostringstream oss;
  oss << "You are working on autopilot project '" << project_name << "'.\n";
  oss << "Working directory: " << selected_path.path.string() << "\n";
  oss << "Managed path name: " << selected_path.name << "\n";
  oss << "Selected task: " << task.title << "\n";
  oss << "Source line in TODO.md: " << task.source_line << "\n";
  oss << "Original TODO line: " << task.source_text << "\n";
  oss << "Implement the selected task if possible.\n";
  oss << "You may inspect and edit files in the working directory.\n";
  oss << "At the end, print a short summary of what you changed.\n";
  return oss.str();
}

void write_text_file(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
  out << content;
}

SelectedProjectPath resolve_project_path(
    const fs::path& projects_file, const std::string& project_name) {
  const std::vector<ProjectPathEntry> entries = load_project_path_entries(projects_file, project_name);
  if (entries.empty()) {
    throw std::runtime_error("no managed path");
  }

  for (const auto& entry : entries) {
    if (entry.name == "main") {
      return SelectedProjectPath{entry.name, fs::path(entry.path)};
    }
  }

  if (entries.size() == 1) {
    return SelectedProjectPath{entries.front().name, fs::path(entries.front().path)};
  }

  throw std::runtime_error("project has multiple paths and no 'main'");
}

std::string sanitize_tmux_name(std::string s) {
  for (char& ch : s) {
    const bool is_alnum =
        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    if (!is_alnum && ch != '-' && ch != '_') {
      ch = '-';
    }
  }
  return s;
}

bool tmux_is_available() {
  return std::getenv("AUTOPILOT_START_DISABLE_TMUX") == nullptr &&
         std::system("tmux -V >/dev/null 2>&1") == 0;
}

void touch_empty_file(const fs::path& path) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to create file: " + path.string());
  }
}

int read_exit_code_file(const fs::path& exit_code_file) {
  std::ifstream in(exit_code_file);
  if (!in) {
    throw std::runtime_error("failed to read exit code file: " + exit_code_file.string());
  }
  int exit_code = 1;
  in >> exit_code;
  return exit_code;
}

AgentLaunchResult run_agent_in_tmux(
    const std::string& project_name,
    const std::string& run_id,
    const std::string& agent_name,
    const std::string& prompt,
    const fs::path& working_directory,
    const fs::path& stdout_log,
    const fs::path& stderr_log) {
  const std::string session = "autopilot";
  const std::string window_name = sanitize_tmux_name("start-" + project_name + "-" + run_id);
  const std::string target = session + ":" + window_name;
  const std::string channel = sanitize_tmux_name("ap-start-" + project_name + "-" + run_id);
  const fs::path exit_code_file = stdout_log.parent_path() / "agent.exit";

  touch_empty_file(stdout_log);
  touch_empty_file(stderr_log);
  touch_empty_file(exit_code_file);

  const bool session_exists =
      std::system(("tmux has-session -t " + session + " >/dev/null 2>&1").c_str()) == 0;

  const std::string agent_command =
      "cd " + shell_quote(working_directory.string()) + " && " +
      build_agent_shell_command(agent_name, prompt) + " > >(tee " + shell_quote(stdout_log.string()) +
      ") 2> >(tee " + shell_quote(stderr_log.string()) + " >&2)";
  const std::string worker_script =
      "set +e; " + agent_command + "; status=$?; printf '%s\\n' \"$status\" > " +
      shell_quote(exit_code_file.string()) + "; tmux wait-for -S " + shell_quote(channel) +
      "; tmux kill-window -t " + shell_quote(target);
  const std::string tmux_command = "bash -lc " + shell_quote(worker_script);

  if (!session_exists) {
    const std::string create_session_cmd =
        "tmux new-session -d -s " + session + " -n " + shell_quote(window_name) + " " +
        shell_quote(tmux_command);
    if (std::system(create_session_cmd.c_str()) != 0) {
      throw std::runtime_error("failed to create tmux session");
    }
  } else {
    const std::string create_window_cmd =
        "tmux new-window -d -t " + session + " -n " + shell_quote(window_name) + " " +
        shell_quote(tmux_command);
    if (std::system(create_window_cmd.c_str()) != 0) {
      throw std::runtime_error("failed to create tmux window");
    }
  }

  if (std::getenv("TMUX") != nullptr) {
    const std::string switch_cmd = "tmux switch-client -t " + shell_quote(target);
    if (std::system(switch_cmd.c_str()) != 0) {
      throw std::runtime_error("failed to switch tmux client");
    }
    if (std::system(("tmux wait-for " + shell_quote(channel)).c_str()) != 0) {
      throw std::runtime_error("failed to wait for tmux task completion");
    }
  } else if (!session_exists) {
    const std::string attach_cmd = "tmux attach-session -t " + shell_quote(target);
    if (std::system(attach_cmd.c_str()) != 0) {
      throw std::runtime_error("failed to attach tmux session");
    }
  } else {
    std::cout << "started task window: " << target << '\n';
    if (std::system(("tmux wait-for " + shell_quote(channel)).c_str()) != 0) {
      throw std::runtime_error("failed to wait for tmux task completion");
    }
  }

  const int exit_code = read_exit_code_file(exit_code_file);
  fs::remove(exit_code_file);
  return AgentLaunchResult{agent_name, exit_code};
}

std::string resolve_project_name(const std::optional<std::string>& maybe_project_name) {
  if (!autopilot_dir_exists()) {
    throw std::runtime_error("Please run ap init first");
  }

  const fs::path projects_file = projects_file_path();
  if (!fs::exists(projects_file)) {
    throw std::runtime_error("project not found");
  }

  const std::set<std::string> existing_projects = load_top_level_projects(projects_file);
  if (existing_projects.empty()) {
    throw std::runtime_error("project not found");
  }

  if (maybe_project_name.has_value()) {
    if (!is_valid_project_name(*maybe_project_name)) {
      throw std::runtime_error("invalid project name");
    }
    if (existing_projects.find(*maybe_project_name) == existing_projects.end()) {
      throw std::runtime_error("project not found");
    }
    return *maybe_project_name;
  }

  const std::optional<std::string> selected_project = select_project_to_start(existing_projects);
  if (!selected_project.has_value()) {
    throw std::runtime_error("failed to read project selection");
  }
  return *selected_project;
}

std::string build_meta_json(
    const std::string& run_id,
    const std::string& project_name,
    const TaskState& task,
    const SelectedProjectPath& selected_path,
    const std::string& agent_name,
    const int attempt_number,
    const std::string& status,
    const std::string& started_at,
    const std::optional<std::string>& ended_at,
    const std::optional<int>& exit_code) {
  std::ostringstream oss;
  oss << "{\n";
  oss << "  \"run_id\": " << json_string(run_id) << ",\n";
  oss << "  \"task_id\": " << json_string(task.id) << ",\n";
  oss << "  \"attempt_number\": " << attempt_number << ",\n";
  oss << "  \"project\": " << json_string(project_name) << ",\n";
  oss << "  \"task_title\": " << json_string(task.title) << ",\n";
  oss << "  \"task_source_file\": " << json_string(task.source_file) << ",\n";
  oss << "  \"task_source_line\": " << task.source_line << ",\n";
  oss << "  \"task_original_line\": " << json_string(task.source_text) << ",\n";
  oss << "  \"path_name\": " << json_string(selected_path.name) << ",\n";
  oss << "  \"working_directory\": " << json_string(selected_path.path.string()) << ",\n";
  oss << "  \"agent\": " << json_string(agent_name) << ",\n";
  oss << "  \"status\": " << json_string(status) << ",\n";
  oss << "  \"started_at\": " << json_string(started_at) << ",\n";
  oss << "  \"ended_at\": " << json_nullable_string(ended_at) << ",\n";
  oss << "  \"exit_code\": ";
  if (exit_code.has_value()) {
    oss << *exit_code;
  } else {
    oss << "null";
  }
  oss << "\n}\n";
  return oss.str();
}

std::string build_result_json(
    const std::string& run_id,
    const TaskState& task,
    const int attempt_number,
    const std::string& process_status,
    const int process_exit_code,
    const std::string& started_at,
    const std::string& ended_at,
    const long long duration_ms,
    const bool todo_update_applied,
    const std::string& final_task_status,
    const std::string& summary_excerpt,
    const std::optional<std::string>& blocker_reason,
    const std::optional<std::string>& alert_id) {
  std::ostringstream oss;
  oss << "{\n";
  oss << "  \"run_id\": " << json_string(run_id) << ",\n";
  oss << "  \"task_id\": " << json_string(task.id) << ",\n";
  oss << "  \"attempt_number\": " << attempt_number << ",\n";
  oss << "  \"status\": " << json_string(process_status) << ",\n";
  oss << "  \"exit_code\": " << process_exit_code << ",\n";
  oss << "  \"process_status\": " << json_string(process_status) << ",\n";
  oss << "  \"process_exit_code\": " << process_exit_code << ",\n";
  oss << "  \"started_at\": " << json_string(started_at) << ",\n";
  oss << "  \"ended_at\": " << json_string(ended_at) << ",\n";
  oss << "  \"duration_ms\": " << duration_ms << ",\n";
  oss << "  \"final_task_status\": " << json_string(final_task_status) << ",\n";
  oss << "  \"todo_update_applied\": " << (todo_update_applied ? "true" : "false") << ",\n";
  oss << "  \"summary_excerpt\": " << json_string(summary_excerpt) << ",\n";
  oss << "  \"blocker_reason\": " << json_nullable_string(blocker_reason) << ",\n";
  oss << "  \"alert_id\": " << json_nullable_string(alert_id) << "\n";
  oss << "}\n";
  return oss.str();
}

std::vector<TaskStatusChange> recover_stale_in_progress_tasks(
    std::vector<TaskState>& tasks, const std::string& recovered_at) {
  std::vector<TaskStatusChange> changes;
  for (TaskState& task : tasks) {
    if (task.status != "in_progress") {
      continue;
    }
    changes.push_back(TaskStatusChange{task.id, task.status, "failed"});
    task.status = "failed";
    task.last_error = "previous ap start did not finish cleanly";
    task.blocker_reason = std::nullopt;
    task.blocker_category = std::nullopt;
    task.updated_at = recovered_at;
  }
  return changes;
}

TaskState* find_task_by_id(std::vector<TaskState>& tasks, const std::string& task_id) {
  for (TaskState& task : tasks) {
    if (task.id == task_id) {
      return &task;
    }
  }
  return nullptr;
}

std::string build_field_change_object_json(
    const std::vector<TaskFieldChange>& fields,
    const bool previous_values) {
  std::ostringstream oss;
  oss << "{";
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) {
      oss << ", ";
    }
    oss << json_string(fields[i].key) << ": "
        << (previous_values ? fields[i].previous_json : fields[i].current_json);
  }
  oss << "}";
  return oss.str();
}

void append_task_updated_event(
    EventLog& event_log,
    const std::string& project_name,
    const TaskUpdateChange& change) {
  std::vector<std::string> changed_fields;
  for (const TaskFieldChange& field : change.fields) {
    changed_fields.push_back(field.key);
  }

  event_log.append(
      project_name,
      EventRecord{
          change.task_id,
          std::nullopt,
          "task.updated",
          "ap.start",
          {
              EventPayloadField{"changed_fields", json_string_array(changed_fields)},
              EventPayloadField{
                  "previous",
                  build_field_change_object_json(change.fields, true),
              },
              EventPayloadField{
                  "current",
                  build_field_change_object_json(change.fields, false),
              },
          },
      });
}

std::map<std::string, const TaskState*> build_task_index(const std::vector<TaskState>& tasks) {
  std::map<std::string, const TaskState*> task_index;
  for (const TaskState& task : tasks) {
    task_index[task.id] = &task;
  }
  return task_index;
}

void validate_task_state(
    const std::vector<TaskState>& tasks, const std::set<std::string>& valid_path_names) {
  const std::map<std::string, const TaskState*> task_index = build_task_index(tasks);

  for (const TaskState& task : tasks) {
    for (const std::string& related_path : task.related_paths) {
      if (related_path.empty() || valid_path_names.find(related_path) == valid_path_names.end()) {
        throw std::runtime_error("invalid related path in task " + task.id);
      }
    }

    for (const std::string& dependency_id : task.depends_on) {
      if (dependency_id == task.id) {
        throw std::runtime_error("invalid task dependency: " + task.id + " -> " + dependency_id);
      }
      if (task_index.find(dependency_id) == task_index.end()) {
        throw std::runtime_error("invalid task dependency: " + task.id + " -> " + dependency_id);
      }
    }
  }

  std::map<std::string, int> visit_state;
  std::function<void(const TaskState&)> dfs = [&](const TaskState& task) {
    visit_state[task.id] = 1;
    for (const std::string& dependency_id : task.depends_on) {
      const int dependency_state = visit_state[dependency_id];
      if (dependency_state == 1) {
        throw std::runtime_error("dependency cycle detected at " + dependency_id);
      }
      if (dependency_state == 2) {
        continue;
      }
      dfs(*task_index.at(dependency_id));
    }
    visit_state[task.id] = 2;
  };

  for (const TaskState& task : tasks) {
    if (visit_state[task.id] == 0) {
      dfs(task);
    }
  }
}

bool status_is_runnable_candidate(const TaskState& task) {
  return task.status == "todo" || task.status == "failed";
}

bool dependencies_are_resolved(
    const TaskState& task, const std::map<std::string, const TaskState*>& task_index) {
  for (const std::string& dependency_id : task.depends_on) {
    if (task_index.at(dependency_id)->status != "done") {
      return false;
    }
  }
  return true;
}

bool task_matches_selected_path(const TaskState& task, const std::string& selected_path_name) {
  return std::find(task.related_paths.begin(), task.related_paths.end(), selected_path_name) !=
         task.related_paths.end();
}

int runnable_status_rank(const TaskState& task) {
  return task.status == "todo" ? 0 : 1;
}

TaskState* select_runnable_task(std::vector<TaskState>& tasks, const std::string& selected_path_name) {
  const std::map<std::string, const TaskState*> task_index = build_task_index(tasks);
  TaskState* selected = nullptr;
  for (TaskState& task : tasks) {
    if (!task.present_in_todo || !status_is_runnable_candidate(task) || task.approval_required ||
        !task_matches_selected_path(task, selected_path_name) ||
        !dependencies_are_resolved(task, task_index)) {
      continue;
    }

    if (selected == nullptr || task.priority < selected->priority ||
        (task.priority == selected->priority &&
         runnable_status_rank(task) < runnable_status_rank(*selected)) ||
        (task.priority == selected->priority &&
         runnable_status_rank(task) == runnable_status_rank(*selected) &&
         task.source_line < selected->source_line) ||
        (task.priority == selected->priority &&
         runnable_status_rank(task) == runnable_status_rank(*selected) &&
         task.source_line == selected->source_line && task.id < selected->id)) {
      selected = &task;
    }
  }
  return selected;
}

ProjectState build_project_state(
    const std::string& project_name,
    const std::vector<TaskState>& tasks,
    const std::optional<ProjectState>& previous_state,
    const std::optional<std::string>& active_task_id,
    const std::optional<std::string>& last_run_id,
    const std::optional<std::string>& last_run_at,
    const std::string& updated_at) {
  ProjectState project = previous_state.value_or(ProjectState{});
  project.project = project_name;
  project.status = "active";
  project.active_task_id = active_task_id;
  project.last_run_id = last_run_id.has_value()
                            ? last_run_id
                            : (previous_state.has_value() ? previous_state->last_run_id : std::nullopt);
  project.last_run_at = last_run_at.has_value()
                            ? last_run_at
                            : (previous_state.has_value() ? previous_state->last_run_at : std::nullopt);
  project.task_counts = compute_project_task_counts(tasks);
  project.updated_at = updated_at;
  return project;
}

void rollback_prelaunch_task_update(
    const fs::path& tasks_dir,
    const fs::path& project_state_file,
    const std::string& project_name,
    std::vector<TaskState>& tasks,
    TaskState& task,
    const TaskState& previous_task,
    const std::optional<ProjectState>& previous_project_state,
    const std::string& rollback_at) {
  try {
    task = previous_task;
    save_task_state(tasks_dir, task);
    save_project_state(
        project_state_file,
        build_project_state(
            project_name,
            tasks,
            previous_project_state,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            rollback_at));
  } catch (const std::exception&) {
  }
}

void append_status_changed_event(
    EventLog& event_log,
    const std::string& project_name,
    const TaskState& task,
    const std::optional<std::string>& run_id,
    const std::string& from_status,
    const std::string& to_status,
    const std::string& reason) {
  event_log.append(
      project_name,
      EventRecord{
          task.id,
          run_id,
          "task.status_changed",
          "ap.start",
          {
              EventPayloadField{"from", json_string(from_status)},
              EventPayloadField{"to", json_string(to_status)},
              EventPayloadField{"reason", json_string(reason)},
          },
      });
}

void finalize_postrun_failure(
    const fs::path& tasks_dir,
    const fs::path& project_state_file,
    const std::string& project_name,
    std::vector<TaskState>& tasks,
    TaskState& task,
    const std::optional<ProjectState>& previous_project_state,
    const std::string& run_id,
    const std::string& failure_reason,
    const std::string& failed_at) {
  if (task.status != "in_progress") {
    return;
  }

  try {
    task.status = "failed";
    task.last_error = failure_reason;
    task.blocker_reason = std::nullopt;
    task.blocker_category = std::nullopt;
    task.updated_at = failed_at;
    save_task_state(tasks_dir, task);
    save_project_state(
        project_state_file,
        build_project_state(
            project_name,
            tasks,
            previous_project_state,
            std::nullopt,
            run_id,
            failed_at,
            failed_at));
  } catch (const std::exception&) {
  }
}

std::string completed_todo_line(const std::string& line) {
  std::string out = line;
  if (out.rfind("- [ ] ", 0) == 0) {
    out.replace(3, 1, "x");
  }
  return out;
}

} // namespace

int cmd_start(const std::optional<std::string>& maybe_project_name) {
  try {
    const std::string project_name = resolve_project_name(maybe_project_name);
    const fs::path project_dir = project_dir_path(project_name);
    const ProjectConfig project_config =
        load_required_project_config(project_config_file_path(project_name));
    const fs::path projects_file = projects_file_path();
    const SelectedProjectPath selected_path = resolve_project_path(projects_file, project_name);
    const fs::path todo_file = project_dir / "TODO.md";
    if (!fs::exists(todo_file)) {
      std::cerr << "ap start failed: TODO.md not found\n";
      return 1;
    }

    const fs::path runtime_dir = project_dir / "runtime";
    const fs::path runs_dir = runtime_dir / "runs";
    const fs::path events_file = runtime_dir / "events" / "events.jsonl";
    const fs::path state_dir = runtime_dir / "state";
    const fs::path project_state_file = state_dir / "project.json";
    const fs::path tasks_dir = state_dir / "tasks";
    const fs::path alerts_dir = runtime_dir / "alerts";
    const fs::path last_run_file = runtime_dir / "last_run.json";
    fs::create_directories(runs_dir);
    fs::create_directories(tasks_dir);
    fs::create_directories(events_file.parent_path());

    EventLog event_log(events_file);
    const std::optional<ProjectState> previous_project_state = load_project_state(project_state_file);
    const std::vector<ProjectPathEntry> project_paths =
        load_project_path_entries(projects_file, project_name);
    std::set<std::string> valid_path_names;
    for (const ProjectPathEntry& path_entry : project_paths) {
      valid_path_names.insert(path_entry.name);
    }

    const std::vector<TaskState> existing_tasks = load_task_states(tasks_dir);
    const std::string synced_at = current_timestamp_with_offset();

    TodoSyncResult sync_result;
    try {
      sync_result = sync_todo_with_task_state(
          todo_file, existing_tasks, synced_at, selected_path.name, project_config.slug);
    } catch (const std::runtime_error& e) {
      event_log.append(
          project_name,
          EventRecord{
              std::nullopt,
              std::nullopt,
              "todo.sync_conflict",
              "ap.start",
              {EventPayloadField{"message", json_string(e.what())}},
          });
      std::cerr << "ap start failed: " << e.what() << '\n';
      return 1;
    }

    const std::vector<TaskStatusChange> recovered_in_progress_changes =
        recover_stale_in_progress_tasks(sync_result.tasks, synced_at);
    validate_task_state(sync_result.tasks, valid_path_names);
    save_task_states(tasks_dir, sync_result.tasks);
    for (const TaskState& task : sync_result.discovered_tasks) {
      event_log.append(
          project_name,
          EventRecord{
              task.id,
              std::nullopt,
              "task.discovered",
              "ap.start",
              {
                  EventPayloadField{"title", json_string(task.title)},
                  EventPayloadField{"source_line", std::to_string(task.source_line)},
              },
          });
    }
    for (const TaskUpdateChange& change : sync_result.task_updates) {
      append_task_updated_event(event_log, project_name, change);
    }
    for (const TaskStatusChange& change : sync_result.status_changes) {
      TaskState* changed_task = find_task_by_id(sync_result.tasks, change.task_id);
      if (changed_task == nullptr) {
        continue;
      }
      append_status_changed_event(
          event_log,
          project_name,
          *changed_task,
          std::nullopt,
          change.from_status,
          change.to_status,
          "todo_sync");
    }
    for (const TaskStatusChange& change : recovered_in_progress_changes) {
      TaskState* changed_task = find_task_by_id(sync_result.tasks, change.task_id);
      if (changed_task == nullptr) {
        continue;
      }
      append_status_changed_event(
          event_log,
          project_name,
          *changed_task,
          std::nullopt,
          change.from_status,
          change.to_status,
          "stale_run_recovered");
    }

    ProjectState project_state = build_project_state(
        project_name,
        sync_result.tasks,
        previous_project_state,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        synced_at);
    save_project_state(project_state_file, project_state);

    TaskState* selected_task = select_runnable_task(sync_result.tasks, selected_path.name);
    if (selected_task == nullptr) {
      std::cerr << "ap start failed: no runnable task\n";
      return 1;
    }

    const std::string agent_name = resolve_agent_name();
    const std::string run_id = allocate_run_id(runs_dir, selected_task->source_line);
    const fs::path run_dir = runs_dir / run_id;
    fs::create_directories(run_dir);

    const fs::path meta_file = run_dir / "meta.json";
    const fs::path prompt_file = run_dir / "prompt.txt";
    const fs::path stdout_file = run_dir / "stdout.log";
    const fs::path stderr_file = run_dir / "stderr.log";
    const fs::path result_file = run_dir / "result.json";

    const std::string started_at = current_timestamp_with_offset();
    const TaskState previous_task_state = *selected_task;
    const std::string previous_status = selected_task->status;
    selected_task->status = "in_progress";
    ++selected_task->attempt_count;
    selected_task->latest_run_id = run_id;
    selected_task->last_error = std::nullopt;
    selected_task->blocker_reason = std::nullopt;
    selected_task->blocker_category = std::nullopt;
    selected_task->updated_at = started_at;
    save_task_state(tasks_dir, *selected_task);

    project_state = build_project_state(
        project_name,
        sync_result.tasks,
        previous_project_state,
        selected_task->id,
        std::nullopt,
        std::nullopt,
        started_at);
    AgentLaunchResult launch_result{agent_name, 1};
    bool launch_started = false;
    const std::string prompt = build_prompt(project_name, selected_path, *selected_task);
    std::chrono::steady_clock::time_point started_clock;
    try {
      save_project_state(project_state_file, project_state);

      event_log.append(
          project_name,
          EventRecord{
              selected_task->id,
              run_id,
              "task.selected",
              "ap.start",
              {
                  EventPayloadField{"reason", json_string("first_runnable_task")},
                  EventPayloadField{"previous_status", json_string(previous_status)},
                  EventPayloadField{"source_line", std::to_string(selected_task->source_line)},
                  EventPayloadField{
                      "attempt_number",
                      std::to_string(selected_task->attempt_count),
                  },
              },
          });
      append_status_changed_event(
          event_log,
          project_name,
          *selected_task,
          run_id,
          previous_status,
          selected_task->status,
          "run_started");

      write_text_file(prompt_file, prompt);
      write_text_file(
          meta_file,
          build_meta_json(
              run_id,
              project_name,
              *selected_task,
              selected_path,
              agent_name,
              selected_task->attempt_count,
              "running",
              started_at,
              std::nullopt,
              std::nullopt));
      event_log.append(
          project_name,
          EventRecord{
              selected_task->id,
              run_id,
              "run.started",
              "ap.start",
              {
                  EventPayloadField{"agent", json_string(agent_name)},
                  EventPayloadField{"path_name", json_string(selected_path.name)},
                  EventPayloadField{
                      "working_directory",
                      json_string(selected_path.path.string()),
                  },
                  EventPayloadField{
                      "attempt_number",
                      std::to_string(selected_task->attempt_count),
                  },
              },
          });

      started_clock = std::chrono::steady_clock::now();
      launch_started = true;
      launch_result = tmux_is_available()
                          ? run_agent_in_tmux(
                                project_name,
                                run_id,
                                agent_name,
                                prompt,
                                selected_path.path,
                                stdout_file,
                                stderr_file)
                          : run_agent(
                                agent_name,
                                prompt,
                                selected_path.path,
                                stdout_file,
                                stderr_file);
    } catch (const std::exception&) {
      if (!launch_started) {
        rollback_prelaunch_task_update(
            tasks_dir,
            project_state_file,
            project_name,
            sync_result.tasks,
            *selected_task,
            previous_task_state,
            previous_project_state,
            current_timestamp_with_offset());
      }
      throw;
    }

    const auto ended_clock = std::chrono::steady_clock::now();
    const std::string ended_at = current_timestamp_with_offset();
    const long long duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     ended_clock - started_clock)
                                     .count();
    const std::string pre_final_status = selected_task->status;

    RunResultClassification classification;
    bool succeeded = false;
    bool blocked = false;
    bool todo_update_applied = false;
    std::optional<AlertRecord> created_alert;
    try {
      classification = classify_run_result(launch_result.exit_code, stdout_file, stderr_file);
      succeeded = classification.final_task_status == "done";
      blocked = classification.final_task_status == "blocked";

      selected_task->status = classification.final_task_status;
      selected_task->updated_at = ended_at;
      if (classification.final_task_status == "done") {
        selected_task->last_error = std::nullopt;
        selected_task->blocker_reason = std::nullopt;
        selected_task->blocker_category = std::nullopt;
      } else if (blocked) {
        selected_task->last_error = classification.blocker_reason;
        selected_task->blocker_reason = classification.blocker_reason;
        selected_task->blocker_category = classification.blocker_category;
        if (classification.approval_required) {
          selected_task->approval_required = true;
        }
      } else {
        selected_task->last_error =
            std::optional<std::string>("agent exited with status " +
                                       std::to_string(launch_result.exit_code));
        selected_task->blocker_reason = std::nullopt;
        selected_task->blocker_category = std::nullopt;
      }
      save_task_state(tasks_dir, *selected_task);

      project_state = build_project_state(
          project_name,
          sync_result.tasks,
          previous_project_state,
          std::nullopt,
          run_id,
          ended_at,
          ended_at);
      save_project_state(project_state_file, project_state);

      if (succeeded) {
        todo_update_applied = mark_todo_task_done(
            todo_file,
            TodoTaskSelection{
                selected_task->id,
                selected_task->title,
                selected_task->source_line,
                selected_task->source_text,
                false,
            });
        if (!todo_update_applied) {
          event_log.append(
              project_name,
              EventRecord{
                  selected_task->id,
                  run_id,
                  "todo.sync_conflict",
                  "ap.start",
                  {EventPayloadField{"message", json_string("failed to update TODO.md after run")}},
              });
        } else {
          selected_task->source_text = completed_todo_line(selected_task->source_text);
          save_task_state(tasks_dir, *selected_task);
        }
      }

      write_text_file(
          meta_file,
          build_meta_json(
              run_id,
              project_name,
              *selected_task,
              selected_path,
              launch_result.agent_name,
              selected_task->attempt_count,
              classification.process_status,
              started_at,
              ended_at,
              launch_result.exit_code));

      event_log.append_stream_file(
          project_name,
          selected_task->id,
          run_id,
          "agent." + launch_result.agent_name,
          "stdout",
          stdout_file);
      event_log.append_stream_file(
          project_name,
          selected_task->id,
          run_id,
          "agent." + launch_result.agent_name,
          "stderr",
          stderr_file);

      event_log.append(
          project_name,
          EventRecord{
              selected_task->id,
              run_id,
              "run.finished",
              "ap.start",
              {
                  EventPayloadField{"agent", json_string(launch_result.agent_name)},
                  EventPayloadField{"exit_code", std::to_string(launch_result.exit_code)},
                  EventPayloadField{"duration_ms", std::to_string(duration_ms)},
              },
          });

      if (blocked) {
        if (classification.alert.has_value()) {
          AlertStore alert_store(alerts_dir);
          created_alert = alert_store.create(
              project_name, selected_task->id, run_id, *classification.alert, ended_at);
        }

        event_log.append(
            project_name,
            EventRecord{
                selected_task->id,
                run_id,
                "task.blocked",
                "runtime.classifier",
                {
                    EventPayloadField{
                        "reason",
                        json_string(classification.blocker_reason.value_or("blocked")),
                    },
                    EventPayloadField{
                        "category",
                        json_string(classification.blocker_category.value_or("blocked")),
                    },
                    EventPayloadField{
                        "approval_required",
                        classification.approval_required ? "true" : "false",
                    },
                    EventPayloadField{
                        "alert_id",
                        json_nullable_string(
                            created_alert.has_value()
                                ? std::optional<std::string>(created_alert->id)
                                : std::nullopt),
                    },
                },
            });

        if (created_alert.has_value()) {
          event_log.append(
              project_name,
              EventRecord{
                  selected_task->id,
                  run_id,
                  "alert.created",
                  "ap.start",
                  {
                      EventPayloadField{"alert_id", json_string(created_alert->id)},
                      EventPayloadField{"severity", json_string(created_alert->severity)},
                      EventPayloadField{"type", json_string(created_alert->type)},
                      EventPayloadField{"message", json_string(created_alert->message)},
                  },
              });
        }
      }

      const std::string result_json = build_result_json(
          run_id,
          *selected_task,
          selected_task->attempt_count,
          classification.process_status,
          launch_result.exit_code,
          started_at,
          ended_at,
          duration_ms,
          todo_update_applied,
          classification.final_task_status,
          classification.summary_excerpt,
          classification.blocker_reason,
          created_alert.has_value() ? std::optional<std::string>(created_alert->id) : std::nullopt);
      write_text_file(result_file, result_json);
      write_text_file(last_run_file, result_json);

      append_status_changed_event(
          event_log,
          project_name,
          *selected_task,
          run_id,
          pre_final_status,
          selected_task->status,
          "result_finalized");
      event_log.append(
          project_name,
          EventRecord{
              selected_task->id,
              run_id,
              "result.final",
              "runtime.classifier",
              {
                  EventPayloadField{
                      "final_task_status",
                      json_string(classification.final_task_status),
                  },
                  EventPayloadField{
                      "process_exit_code",
                      std::to_string(launch_result.exit_code),
                  },
                  EventPayloadField{
                      "process_status",
                      json_string(classification.process_status),
                  },
                  EventPayloadField{
                      "summary",
                      json_string(classification.summary_excerpt),
                  },
                  EventPayloadField{
                      "todo_update_applied",
                      todo_update_applied ? "true" : "false",
                  },
                  EventPayloadField{
                      "alert_id",
                      json_nullable_string(
                          created_alert.has_value()
                              ? std::optional<std::string>(created_alert->id)
                              : std::nullopt),
                  },
              },
          });
    } catch (const std::exception& e) {
      finalize_postrun_failure(
          tasks_dir,
          project_state_file,
          project_name,
          sync_result.tasks,
          *selected_task,
          previous_project_state,
          run_id,
          "post-run processing failed: " + std::string(e.what()),
          current_timestamp_with_offset());
      throw;
    }

    if (succeeded) {
      std::cout << "completed task: " << selected_task->title << '\n';
      return 0;
    }

    if (blocked) {
      std::cerr << "ap start blocked: "
                << classification.blocker_reason.value_or("blocked by external dependency") << '\n';
      return 1;
    }

    std::cerr << "ap start failed: agent exited with status " << launch_result.exit_code << '\n';
    return 1;
  } catch (const std::exception& e) {
    const std::string message = e.what();
    if (message == "Please run ap init first" || message == "project not found" ||
        message == "invalid project name") {
      std::cerr << message << '\n';
    } else if (message == "no managed path" || message == "project has multiple paths and no 'main'") {
      std::cerr << "ap start failed: " << message << '\n';
    } else {
      std::cerr << "ap start failed: " << message << '\n';
    }
    return 1;
  }
}
