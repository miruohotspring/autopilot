#include "autopilot/runtime/workflow.hpp"

#include "autopilot/commands/delete_ui.hpp"
#include "autopilot/projects/project_paths.hpp"
#include "autopilot/projects/todo_task_selector.hpp"
#include "autopilot/runtime/alert_store.hpp"
#include "autopilot/runtime/json_utils.hpp"
#include "autopilot/runtime/lock_manager.hpp"
#include "autopilot/runtime/run_result_classifier.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
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
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

std::optional<RunMetaData> parse_run_meta_file(const fs::path& meta_file);
TaskState* find_task_by_id(std::vector<TaskState>& tasks, const std::string& task_id);

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

void finalize_interrupted_reviewer_run(
    const fs::path& runs_dir,
    const RunMetaData& meta,
    const std::string& ended_at) {
  if (meta.status != "starting" && meta.status != "running") {
    return;
  }

  RunMetaData finished_meta = meta;
  finished_meta.status = "finished";
  finished_meta.ended_at = ended_at;
  finished_meta.exit_reason = "internal_error";
  write_text_file(runs_dir / meta.run_id / "meta.json", build_meta_json(finished_meta));
}

std::vector<TaskStatusChange> recover_stale_tasks(
    std::vector<TaskState>& tasks, const fs::path& runs_dir, const std::string& recovered_at) {
  std::vector<TaskStatusChange> changes;
  for (TaskState& task : tasks) {
    if (task.status != "in_progress") {
      continue;
    }
    if (task.latest_run_id.has_value()) {
      if (const std::optional<RunMetaData> meta =
              parse_run_meta_file(runs_dir / *task.latest_run_id / "meta.json");
          meta.has_value() && meta->status == "published") {
        continue;
      }
    }
    changes.push_back(TaskStatusChange{task.id, task.status, "failed"});
    task.status = "failed";
    task.last_run_exit_reason = "failed";
    task.last_error = "previous ap start did not finish cleanly";
    task.blocker_reason = std::nullopt;
    task.blocker_category = std::nullopt;
    task.updated_at = recovered_at;
  }

  for (TaskState& task : tasks) {
    if (task.status != "review_pending") {
      continue;
    }
    if (!task.reviewer_run_id.has_value()) {
      continue;
    }
    if (task.reviewer_run_id.has_value()) {
      if (const std::optional<RunMetaData> meta =
              parse_run_meta_file(runs_dir / *task.reviewer_run_id / "meta.json");
          meta.has_value() && meta->role == "reviewer") {
        if (meta->status == "published") {
          continue;
        }
        finalize_interrupted_reviewer_run(runs_dir, *meta, recovered_at);
        task.latest_run_id = meta->run_id;
        task.reviewer_run_id = meta->run_id;
      } else {
        task.latest_run_id = task.reviewer_run_id;
      }
    }
    changes.push_back(TaskStatusChange{task.id, task.status, "blocked"});
    task.status = "blocked";
    task.last_run_exit_reason = "internal_error";
    task.review_result = "reviewer_error";
    task.last_error = "previous reviewer run did not finish cleanly";
    task.blocker_reason = "previous reviewer run did not finish cleanly";
    task.blocker_category = "reviewer_error";
    task.updated_at = recovered_at;
  }

  for (const auto& entry : fs::directory_iterator(runs_dir)) {
    if (!entry.is_directory()) {
      continue;
    }
    const std::optional<RunMetaData> meta = parse_run_meta_file(entry.path() / "meta.json");
    if (!meta.has_value() || meta->role != "reviewer") {
      continue;
    }
    if (meta->status != "starting" && meta->status != "running") {
      continue;
    }
    TaskState* task = find_task_by_id(tasks, meta->task_id);
    if (task == nullptr) {
      continue;
    }
    if (task->latest_run_id == meta->run_id || task->reviewer_run_id == meta->run_id) {
      continue;
    }
    finalize_interrupted_reviewer_run(runs_dir, *meta, recovered_at);
    changes.push_back(TaskStatusChange{task->id, task->status, "blocked"});
    task->status = "blocked";
    task->latest_run_id = meta->run_id;
    task->reviewer_run_id = meta->run_id;
    task->last_run_exit_reason = "internal_error";
    task->review_result = "reviewer_error";
    task->last_error = "previous reviewer run did not finish cleanly";
    task->blocker_reason = "previous reviewer run did not finish cleanly";
    task->blocker_category = "reviewer_error";
    task->updated_at = recovered_at;
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
    const TaskUpdateChange& change,
    const std::string& actor) {
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
          actor,
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
  if (task.status == "todo") {
    return true;
  }
  if (task.status == "failed") {
    if (task.attempt_count >= task.max_retries) {
      return false;
    }
    if (!task.last_run_exit_reason.has_value()) {
      return true;
    }
    return std::find(
               task.retry_on.begin(),
               task.retry_on.end(),
               *task.last_run_exit_reason) != task.retry_on.end();
  }
  return false;
}

bool is_retry_exhausted(const TaskState& task) {
  return task.status == "failed" && task.attempt_count >= task.max_retries;
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

std::string coder_logical_agent_name(const std::string& agent_name) {
  return "coder." + agent_name;
}

std::string reviewer_logical_agent_name(const std::string& agent_name) {
  return "reviewer." + agent_name;
}

bool review_flow_enabled_for_task(
    const TaskState& task,
    const ProjectState& project_state,
    const std::optional<bool>& maybe_review_enabled) {
  if (task.skip_review) {
    return false;
  }
  if (task.review_required) {
    return true;
  }
  if (maybe_review_enabled.has_value()) {
    return *maybe_review_enabled;
  }
  return project_state.review_enabled;
}

int effective_max_review_cycles(const TaskState& task, const ProjectState& project_state) {
  return task.max_review_cycles.value_or(project_state.max_review_cycles);
}

std::string reviewer_failure_exit_reason(const int exit_code) {
  return exit_code >= 128 ? "signal" : "non_zero_exit";
}

int runnable_status_rank(const TaskState& task) {
  return task.status == "todo" ? 0 : 1;
}

TaskState* select_runnable_task(
    std::vector<TaskState>& tasks,
    const std::string& selected_path_name,
    int& retry_exhausted_count) {
  retry_exhausted_count = 0;
  const std::map<std::string, const TaskState*> task_index = build_task_index(tasks);
  TaskState* selected = nullptr;
  for (TaskState& task : tasks) {
    if (!task.present_in_todo || task.approval_required ||
        !task_matches_selected_path(task, selected_path_name) ||
        !dependencies_are_resolved(task, task_index)) {
      continue;
    }

    // Count tasks excluded due to retry exhaustion
    if (is_retry_exhausted(task)) {
      ++retry_exhausted_count;
      continue;
    }

    if (!status_is_runnable_candidate(task)) {
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
    const std::optional<std::string>& active_run_id,
    const std::optional<std::string>& last_run_id,
    const std::optional<std::string>& last_run_at,
    int run_counter,
    int default_timeout_seconds,
    const std::string& updated_at) {
  ProjectState project = previous_state.value_or(ProjectState{});
  project.project = project_name;
  project.status = "active";
  project.active_task_id = active_task_id;
  project.active_run_id = active_run_id;
  project.last_run_id = last_run_id.has_value()
                            ? last_run_id
                            : (previous_state.has_value() ? previous_state->last_run_id : std::nullopt);
  project.last_run_at = last_run_at.has_value()
                            ? last_run_at
                            : (previous_state.has_value() ? previous_state->last_run_at : std::nullopt);
  project.run_counter = run_counter;
  project.default_timeout_seconds = default_timeout_seconds;
  project.review_enabled =
      previous_state.has_value() ? previous_state->review_enabled : false;
  project.max_review_cycles =
      previous_state.has_value() ? previous_state->max_review_cycles : 2;
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
    int run_counter,
    int default_timeout_seconds,
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
            std::nullopt,
            run_counter,
            default_timeout_seconds,
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
    const std::string& reason,
    const std::string& actor) {
  event_log.append(
      project_name,
      EventRecord{
          task.id,
          run_id,
          "task.status_changed",
          actor,
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
    int run_counter,
    int default_timeout_seconds,
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
            std::nullopt,
            run_id,
            failed_at,
            run_counter,
            default_timeout_seconds,
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

bool is_supported_actor_name(const std::string& actor_name) {
  return actor_name == "claude" || actor_name == "codex" || actor_name == "human";
}

RuntimeContext load_runtime_context(const std::optional<std::string>& maybe_project_name) {
  RuntimeContext context;
  context.project_name = resolve_project_name(maybe_project_name);
  context.project_dir = project_dir_path(context.project_name);
  context.projects_file = projects_file_path();
  context.project_config =
      load_required_project_config(context.projects_file, context.project_name);
  context.selected_path = resolve_project_path(context.projects_file, context.project_name);
  context.todo_file = context.project_dir / "TODO.md";
  if (!fs::exists(context.todo_file)) {
    throw std::runtime_error("TODO.md not found");
  }
  context.runtime_dir = context.project_dir / "runtime";
  context.runs_dir = context.runtime_dir / "runs";
  context.events_file = context.runtime_dir / "events" / "events.jsonl";
  context.state_dir = context.runtime_dir / "state";
  context.project_state_file = context.state_dir / "project.json";
  context.tasks_dir = context.state_dir / "tasks";
  context.lock_dir = context.runtime_dir / "lock";
  context.alerts_dir = context.runtime_dir / "alerts";
  context.last_run_file = context.runtime_dir / "last_run.json";
  fs::create_directories(context.runs_dir);
  fs::create_directories(context.tasks_dir);
  fs::create_directories(context.events_file.parent_path());
  fs::create_directories(context.lock_dir);
  context.previous_project_state = load_project_state(context.project_state_file);
  context.run_counter =
      context.previous_project_state.has_value() ? context.previous_project_state->run_counter : 0;
  context.default_timeout_seconds = context.previous_project_state.has_value()
                                        ? context.previous_project_state->default_timeout_seconds
                                        : 1800;
  return context;
}

SyncedContext load_synced_context(
    const std::optional<std::string>& maybe_project_name, const std::string& actor) {
  SyncedContext synced;
  synced.runtime = load_runtime_context(maybe_project_name);
  synced.existing_tasks = load_task_states(synced.runtime.tasks_dir);
  synced.synced_at = current_timestamp_with_offset();
  EventLog event_log(synced.runtime.events_file);
  try {
    synced.sync_result = sync_todo_with_task_state(
        synced.runtime.todo_file,
        synced.existing_tasks,
        synced.synced_at,
        synced.runtime.selected_path.name,
        synced.runtime.project_config.slug);
  } catch (const std::runtime_error& e) {
    event_log.append(
        synced.runtime.project_name,
        EventRecord{
            std::nullopt,
            std::nullopt,
            "todo.sync_conflict",
            actor,
            {EventPayloadField{"message", json_string(e.what())}},
        });
    throw;
  }

  const std::vector<ProjectPathEntry> project_paths =
      load_project_path_entries(synced.runtime.projects_file, synced.runtime.project_name);
  std::set<std::string> valid_path_names;
  for (const ProjectPathEntry& path_entry : project_paths) {
    valid_path_names.insert(path_entry.name);
  }
  validate_task_state(synced.sync_result.tasks, valid_path_names);
  return synced;
}

ProjectState persist_synced_state(
    const RuntimeContext& runtime,
    EventLog& event_log,
    TodoSyncResult& sync_result,
    const std::vector<TaskStatusChange>& recovered_in_progress_changes,
    const int run_counter,
    const std::string& updated_at,
    const std::string& actor) {
  std::optional<std::string> recovered_last_run_id;
  std::optional<std::string> recovered_last_run_at;

  save_task_states(runtime.tasks_dir, sync_result.tasks);
  for (const TaskState& task : sync_result.discovered_tasks) {
    event_log.append(
        runtime.project_name,
        EventRecord{
            task.id,
            std::nullopt,
            "task.discovered",
            actor,
            {
                EventPayloadField{"title", json_string(task.title)},
                EventPayloadField{"source_line", std::to_string(task.source_line)},
            },
        });
  }
  for (const TaskUpdateChange& change : sync_result.task_updates) {
    append_task_updated_event(event_log, runtime.project_name, change, actor);
  }
  for (const TaskStatusChange& change : sync_result.status_changes) {
    TaskState* changed_task = find_task_by_id(sync_result.tasks, change.task_id);
    if (changed_task == nullptr) {
      continue;
    }
    append_status_changed_event(
        event_log,
        runtime.project_name,
        *changed_task,
        std::nullopt,
        change.from_status,
        change.to_status,
        "todo_sync",
        actor);
  }
  for (const TaskStatusChange& change : recovered_in_progress_changes) {
    TaskState* changed_task = find_task_by_id(sync_result.tasks, change.task_id);
    if (changed_task == nullptr) {
      continue;
    }
    append_status_changed_event(
        event_log,
        runtime.project_name,
        *changed_task,
        std::nullopt,
        change.from_status,
        change.to_status,
        "stale_run_recovered",
        actor);
  }
  for (const TaskStatusChange& change : recovered_in_progress_changes) {
    TaskState* changed_task = find_task_by_id(sync_result.tasks, change.task_id);
    if (changed_task == nullptr) {
      continue;
    }
    const std::optional<std::string> interrupted_run_id = changed_task->latest_run_id;
    event_log.append(
        runtime.project_name,
        EventRecord{
            changed_task->id,
            interrupted_run_id,
            "run.interrupted",
            actor,
            {
                EventPayloadField{"run_id", json_nullable_string(interrupted_run_id)},
                EventPayloadField{"detected_at", json_string(updated_at)},
            },
        });

    if (changed_task->review_result == "reviewer_error" &&
        changed_task->blocker_reason == "previous reviewer run did not finish cleanly" &&
        changed_task->reviewer_run_id.has_value()) {
      recovered_last_run_id = changed_task->reviewer_run_id;
      recovered_last_run_at = changed_task->updated_at;

      const AlertRecord alert = AlertStore(runtime.alerts_dir).create(
          runtime.project_name,
          changed_task->id,
          *changed_task->reviewer_run_id,
          AlertDraft{
              "medium",
              "reviewer_error",
              "previous reviewer run did not finish cleanly for task " + changed_task->id,
          },
          updated_at);
      event_log.append(
          runtime.project_name,
          EventRecord{
              changed_task->id,
              changed_task->reviewer_run_id,
              "alert.created",
              actor,
              {
                  EventPayloadField{"alert_id", json_string(alert.id)},
                  EventPayloadField{"severity", json_string(alert.severity)},
                  EventPayloadField{"type", json_string(alert.type)},
                  EventPayloadField{"message", json_string(alert.message)},
              },
          });
    }
  }

  const ProjectState project_state = build_project_state(
      runtime.project_name,
      sync_result.tasks,
      runtime.previous_project_state,
      std::nullopt,
      std::nullopt,
      recovered_last_run_id,
      recovered_last_run_at,
      run_counter,
      runtime.default_timeout_seconds,
      updated_at);
  save_project_state(runtime.project_state_file, project_state);
  return project_state;
}

void append_retry_exhausted_events(
    EventLog& event_log,
    const std::string& project_name,
    const std::vector<TaskState>& tasks,
    const std::string& actor,
    std::ostream& diagnostics) {
  for (const TaskState& task : tasks) {
    if (!task.present_in_todo || !is_retry_exhausted(task)) {
      continue;
    }
    diagnostics << "ap start: task " << task.id << " has reached max retries ("
                << task.attempt_count << "/" << task.max_retries << "), skipping\n";
    event_log.append(
        project_name,
        EventRecord{
            task.id,
            std::nullopt,
            "task.retry_exhausted",
            actor,
            {
                EventPayloadField{"attempt_count", std::to_string(task.attempt_count)},
                EventPayloadField{"max_retries", std::to_string(task.max_retries)},
            },
        });
  }
}

std::optional<RunLocator> locate_run_by_id(const std::string& run_id) {
  const fs::path projects_file = projects_file_path();
  const std::set<std::string> projects = load_top_level_projects(projects_file);
  std::vector<RunLocator> matches;
  for (const std::string& project_name : projects) {
    const std::optional<RunMetaData> meta =
        parse_run_meta_file(project_dir_path(project_name) / "runtime" / "runs" / run_id / "meta.json");
    if (!meta.has_value()) {
      continue;
    }
    RunLocator locator;
    locator.runtime = load_runtime_context(project_name);
    locator.meta = *meta;
    locator.run_dir = locator.runtime.runs_dir / run_id;
    locator.meta_file = locator.run_dir / "meta.json";
    matches.push_back(std::move(locator));
  }

  if (matches.empty()) {
    return std::nullopt;
  }

  const auto unfinished = std::find_if(
      matches.begin(), matches.end(), [](const RunLocator& locator) {
        return locator.meta.status != "finished";
      });
  if (unfinished != matches.end()) {
    const auto another_unfinished = std::find_if(
        unfinished + 1, matches.end(), [](const RunLocator& locator) {
          return locator.meta.status != "finished";
        });
    if (another_unfinished != matches.end()) {
      throw std::runtime_error("run id is ambiguous across projects");
    }
    return *unfinished;
  }

  if (matches.size() > 1) {
    throw std::runtime_error("run id is ambiguous across projects");
  }
  return matches.front();
}

std::string normalize_final_status(const std::string& status) {
  if (status == "done" || status == "failed" || status == "blocked" ||
      status == "timeout" || status == "cancelled") {
    return status;
  }
  throw std::runtime_error("invalid run status");
}

std::string normalize_task_status_override(const std::string& status) {
  if (status == "todo" || status == "in_progress" || status == "review_pending" ||
      status == "blocked" || status == "done" || status == "failed" ||
      status == "cancelled") {
    return status;
  }
  throw std::runtime_error("invalid task status");
}

ReviewerVerdict build_cli_reviewer_verdict(
    const std::string& verdict_name,
    const std::optional<std::string>& maybe_summary,
    const std::vector<std::string>& issues,
    const std::vector<std::string>& suggestions,
    const std::optional<std::string>& maybe_reason,
    const std::optional<std::string>& maybe_category) {
  ReviewerVerdict verdict;
  verdict.verdict = verdict_name;
  if (verdict_name == "approve") {
    verdict.summary = maybe_summary.value_or("approved");
    return verdict;
  }
  if (verdict_name == "rework") {
    verdict.summary = maybe_summary;
    verdict.issues = issues;
    verdict.suggestions = suggestions;
    return verdict;
  }
  if (verdict_name == "blocked") {
    verdict.reason = maybe_reason.value_or("blocked");
    verdict.category = maybe_category.value_or("other");
    return verdict;
  }
  throw std::runtime_error("invalid review verdict");
}

PublishedRunInfo publish_coder_run(
    RuntimeContext& runtime,
    EventLog& event_log,
    std::vector<TaskState>& tasks,
    TaskState& task,
    int& run_counter,
    const std::string& logical_agent_name,
    const std::string& meta_status,
    const std::string& event_actor,
    const std::string& run_event_type,
    const std::string& run_event_reason) {
  ++run_counter;
  PublishedRunInfo info;
  info.run_id = allocate_run_id_with_counter(runtime.runs_dir, run_counter);
  info.run_dir = runtime.runs_dir / info.run_id;
  fs::create_directories(info.run_dir);
  info.meta_file = info.run_dir / "meta.json";
  info.prompt_file = info.run_dir / "prompt.txt";
  info.stdout_file = info.run_dir / "stdout.log";
  info.stderr_file = info.run_dir / "stderr.log";
  info.result_file = info.run_dir / "result.json";
  info.timeout_marker_file = info.run_dir / "agent.timeout";
  info.started_at = current_timestamp_with_offset();
  info.previous_status = task.status;
  info.prompt = build_prompt(runtime.project_name, runtime.selected_path, task);

  task.status = "in_progress";
  ++task.attempt_count;
  task.latest_run_id = info.run_id;
  task.last_run_exit_reason = std::nullopt;
  task.last_error = std::nullopt;
  task.blocker_reason = std::nullopt;
  task.blocker_category = std::nullopt;
  task.updated_at = info.started_at;
  save_task_state(runtime.tasks_dir, task);

  const ProjectState project_state = build_project_state(
      runtime.project_name,
      tasks,
      runtime.previous_project_state,
      task.id,
      info.run_id,
      std::nullopt,
      std::nullopt,
      run_counter,
      runtime.default_timeout_seconds,
      info.started_at);
  save_project_state(runtime.project_state_file, project_state);

  write_text_file(info.prompt_file, info.prompt);
  write_text_file(
      info.meta_file,
      build_meta_json(build_run_meta_data(
          info.run_id,
          runtime.project_name,
          task,
          runtime.selected_path,
          logical_agent_name,
          "coder",
          task.attempt_count,
          meta_status,
          info.started_at,
          std::nullopt,
          std::nullopt,
          std::nullopt)));

  event_log.append(
      runtime.project_name,
      EventRecord{
          task.id,
          info.run_id,
          "task.selected",
          event_actor,
          {
              EventPayloadField{"reason", json_string(run_event_reason)},
              EventPayloadField{"previous_status", json_string(info.previous_status)},
              EventPayloadField{"source_line", std::to_string(task.source_line)},
              EventPayloadField{"attempt_number", std::to_string(task.attempt_count)},
          },
      });
  append_status_changed_event(
      event_log,
      runtime.project_name,
      task,
      info.run_id,
      info.previous_status,
      task.status,
      meta_status == "published" ? "run_published" : "run_started",
      event_actor);
  event_log.append(
      runtime.project_name,
      EventRecord{
          task.id,
          info.run_id,
          run_event_type,
          event_actor,
          {
              EventPayloadField{"agent", json_string(logical_agent_name)},
              EventPayloadField{"path_name", json_string(runtime.selected_path.name)},
              EventPayloadField{
                  "working_directory",
                  json_string(runtime.selected_path.path.string()),
              },
              EventPayloadField{"attempt", std::to_string(task.attempt_count)},
              EventPayloadField{"role", json_string("coder")},
              EventPayloadField{"prompt_file", json_string(info.prompt_file.string())},
          },
      });

  return info;
}

PublishedRunInfo publish_reviewer_run(
    RuntimeContext& runtime,
    EventLog& event_log,
    std::vector<TaskState>& tasks,
    TaskState& task,
    int& run_counter,
    const std::string& logical_agent_name,
    const std::string& coder_run_id,
    const std::string& coder_exit_reason,
    const std::string& coder_stdout,
    const std::string& meta_status,
    const std::string& event_actor,
    const std::string& run_event_type) {
  ++run_counter;
  PublishedRunInfo info;
  info.run_id = allocate_run_id_with_counter(runtime.runs_dir, run_counter);
  info.run_dir = runtime.runs_dir / info.run_id;
  fs::create_directories(info.run_dir);
  info.meta_file = info.run_dir / "meta.json";
  info.prompt_file = info.run_dir / "prompt.txt";
  info.stdout_file = info.run_dir / "stdout.log";
  info.stderr_file = info.run_dir / "stderr.log";
  info.result_file = info.run_dir / "result.json";
  info.timeout_marker_file = info.run_dir / "agent.timeout";
  info.started_at = current_timestamp_with_offset();
  info.previous_status = task.status;
  const int review_cycle = task.review_cycle_count + 1;
  info.prompt = build_reviewer_prompt(task, coder_run_id, coder_exit_reason, coder_stdout);

  write_text_file(info.prompt_file, info.prompt);
  write_text_file(
      info.meta_file,
      build_meta_json(build_run_meta_data(
          info.run_id,
          runtime.project_name,
          task,
          runtime.selected_path,
          logical_agent_name,
          "reviewer",
          task.attempt_count,
          meta_status,
          info.started_at,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          coder_run_id,
          review_cycle)));

  task.status = "review_pending";
  task.latest_run_id = info.run_id;
  task.reviewer_run_id = info.run_id;
  task.updated_at = info.started_at;
  save_task_state(runtime.tasks_dir, task);

  const ProjectState project_state = build_project_state(
      runtime.project_name,
      tasks,
      runtime.previous_project_state,
      task.id,
      info.run_id,
      coder_run_id,
      std::nullopt,
      run_counter,
      runtime.default_timeout_seconds,
      info.started_at);
  save_project_state(runtime.project_state_file, project_state);

  event_log.append(
      runtime.project_name,
      EventRecord{
          task.id,
          info.run_id,
          run_event_type,
          event_actor,
          {
              EventPayloadField{"coder_run_id", json_string(coder_run_id)},
              EventPayloadField{"review_cycle", std::to_string(review_cycle)},
              EventPayloadField{"agent", json_string(logical_agent_name)},
              EventPayloadField{"prompt_file", json_string(info.prompt_file.string())},
          },
      });
  append_status_changed_event(
      event_log,
      runtime.project_name,
      task,
      info.run_id,
      info.previous_status,
      task.status,
      meta_status == "published" ? "review_published" : "review_started",
      event_actor);
  return info;
}
