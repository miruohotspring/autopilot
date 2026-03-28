#include "autopilot/commands/cmd_start.hpp"

#include "autopilot/agents/agent_launcher.hpp"
#include "autopilot/runtime/json_utils.hpp"
#include "autopilot/runtime/lock_manager.hpp"
#include "autopilot/runtime/workflow.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

int cmd_start(
    const std::optional<std::string>& maybe_project_name,
    std::optional<int> maybe_timeout_seconds,
    std::optional<bool> maybe_review_enabled,
    std::optional<std::string> maybe_reviewer_agent) {
  try {
    SyncedContext synced = load_synced_context(maybe_project_name, "ap.start");
    RuntimeContext& runtime = synced.runtime;
    EventLog event_log(runtime.events_file);
    const int timeout_seconds = maybe_timeout_seconds.value_or(runtime.default_timeout_seconds);

    int retry_exhausted_count = 0;
    const bool has_existing_in_progress = std::any_of(
        synced.existing_tasks.begin(),
        synced.existing_tasks.end(),
        [](const TaskState& task) { return task.status == "in_progress"; });
    const bool has_existing_review_pending = std::any_of(
        synced.existing_tasks.begin(),
        synced.existing_tasks.end(),
        [](const TaskState& task) { return task.status == "review_pending"; });
    bool has_orphaned_reviewer_run = false;
    for (const auto& entry : std::filesystem::directory_iterator(runtime.runs_dir)) {
      if (!entry.is_directory()) {
        continue;
      }
      const std::optional<RunMetaData> meta = parse_run_meta_file(entry.path() / "meta.json");
      if (meta.has_value() && meta->role == "reviewer" &&
          (meta->status == "starting" || meta->status == "running")) {
        has_orphaned_reviewer_run = true;
        break;
      }
    }

    std::vector<TaskStatusChange> recovered_in_progress_changes;
    bool sync_state_persisted = false;
    if (has_existing_in_progress || has_existing_review_pending || has_orphaned_reviewer_run) {
      LockManager sync_lock_manager(runtime.lock_dir);
      bool sync_lock_was_stale = false;
      LockInfo sync_stale_info{};
      const bool sync_lock_acquired = sync_lock_manager.acquire_project_lock(
          allocate_run_id_with_counter(runtime.runs_dir, 0),
          timeout_seconds,
          sync_lock_was_stale,
          sync_stale_info);

      if (sync_lock_was_stale) {
        event_log.append(
            runtime.project_name,
            EventRecord{
                std::nullopt,
                std::nullopt,
                "lock.stale_detected",
                "ap.start",
                {
                    EventPayloadField{"stale_pid", std::to_string(sync_stale_info.pid)},
                    EventPayloadField{"stale_run_id", json_string(sync_stale_info.run_id)},
                },
            });
        std::cerr << "ap start: stale lock detected for pid " << sync_stale_info.pid
                  << ", recovering\n";
      }

      if (!sync_lock_acquired) {
        std::cerr << "ap start failed: project is already locked by pid " << sync_stale_info.pid
                  << " (run: " << sync_stale_info.run_id << ")\n";
        return 1;
      }

      try {
        recovered_in_progress_changes =
            recover_stale_tasks(synced.sync_result.tasks, runtime.runs_dir, synced.synced_at);
        append_retry_exhausted_events(
            event_log, runtime.project_name, synced.sync_result.tasks, "ap.start", std::cerr);
        persist_synced_state(
            runtime,
            event_log,
            synced.sync_result,
            recovered_in_progress_changes,
            runtime.run_counter,
            synced.synced_at,
            "ap.start");
        sync_state_persisted = true;
      } catch (const std::exception&) {
        sync_lock_manager.release_project_lock();
        throw;
      }
      sync_lock_manager.release_project_lock();
    }

    TaskState* selected_task = select_runnable_task(
        synced.sync_result.tasks, runtime.selected_path.name, retry_exhausted_count);
    if (selected_task == nullptr) {
      if (!sync_state_persisted) {
        append_retry_exhausted_events(
            event_log, runtime.project_name, synced.sync_result.tasks, "ap.start", std::cerr);
        persist_synced_state(
            runtime,
            event_log,
            synced.sync_result,
            recovered_in_progress_changes,
            runtime.run_counter,
            synced.synced_at,
            "ap.start");
      }
      if (retry_exhausted_count > 0) {
        std::cerr << "ap start failed: no runnable task (" << retry_exhausted_count << " task"
                  << (retry_exhausted_count == 1 ? "" : "s")
                  << " exhausted max retries)\n";
      } else {
        std::cerr << "ap start failed: no runnable task\n";
      }
      return 1;
    }

    const std::string agent_name = resolve_agent_name(false);
    const std::string reviewer_agent_name =
        resolve_reviewer_agent_name(maybe_reviewer_agent, agent_name, false);
    const std::string expected_run_id =
        allocate_run_id_with_counter(runtime.runs_dir, runtime.run_counter + 1);

    LockManager lock_manager(runtime.lock_dir);
    bool proj_lock_was_stale = false;
    LockInfo proj_stale_info{};
    const bool proj_lock_acquired = lock_manager.acquire_project_lock(
        expected_run_id, timeout_seconds, proj_lock_was_stale, proj_stale_info);
    if (proj_lock_was_stale) {
      event_log.append(
          runtime.project_name,
          EventRecord{
              std::nullopt,
              std::nullopt,
              "lock.stale_detected",
              "ap.start",
              {
                  EventPayloadField{"stale_pid", std::to_string(proj_stale_info.pid)},
                  EventPayloadField{"stale_run_id", json_string(proj_stale_info.run_id)},
              },
          });
      std::cerr << "ap start: stale lock detected for pid " << proj_stale_info.pid
                << ", recovering\n";
    }
    if (!proj_lock_acquired) {
      std::cerr << "ap start failed: project is already locked by pid " << proj_stale_info.pid
                << " (run: " << proj_stale_info.run_id << ")\n";
      return 1;
    }

    event_log.append(
        runtime.project_name,
        EventRecord{
            std::nullopt,
            expected_run_id,
            "lock.acquired",
            "ap.start",
            {
                EventPayloadField{"lock_type", json_string("project")},
                EventPayloadField{"run_id", json_string(expected_run_id)},
            },
        });

    if (!sync_state_persisted) {
      append_retry_exhausted_events(
          event_log, runtime.project_name, synced.sync_result.tasks, "ap.start", std::cerr);
      persist_synced_state(
          runtime,
          event_log,
          synced.sync_result,
          recovered_in_progress_changes,
          runtime.run_counter,
          synced.synced_at,
          "ap.start");
    }

    if (!lock_manager.acquire_task_lock(selected_task->id, expected_run_id, timeout_seconds)) {
      lock_manager.release_project_lock();
      std::cerr << "ap start failed: task " << selected_task->id << " is already locked\n";
      return 1;
    }

    event_log.append(
        runtime.project_name,
        EventRecord{
            selected_task->id,
            expected_run_id,
            "lock.acquired",
            "ap.start",
            {
                EventPayloadField{"lock_type", json_string("task")},
                EventPayloadField{"task_id", json_string(selected_task->id)},
                EventPayloadField{"run_id", json_string(expected_run_id)},
            },
        });

    StartExecutionResult execution;
    try {
      execution = execute_ap_start_locked_run(
          runtime,
          event_log,
          lock_manager,
          synced.sync_result.tasks,
          *selected_task,
          runtime.run_counter,
          timeout_seconds,
          agent_name,
          reviewer_agent_name,
          maybe_review_enabled);
    } catch (const std::exception&) {
      event_log.append(
          runtime.project_name,
          EventRecord{
              selected_task->id,
              expected_run_id,
              "lock.released",
              "ap.start",
              {EventPayloadField{"lock_type", json_string("task")}},
          });
      lock_manager.release_task_lock(selected_task->id);
      event_log.append(
          runtime.project_name,
          EventRecord{
              std::nullopt,
              expected_run_id,
              "lock.released",
              "ap.start",
              {EventPayloadField{"lock_type", json_string("project")}},
          });
      lock_manager.release_project_lock();
      throw;
    }

    event_log.append(
        runtime.project_name,
        EventRecord{
            selected_task->id,
            execution.run_id,
            "lock.released",
            "ap.start",
            {EventPayloadField{"lock_type", json_string("task")}},
        });
    lock_manager.release_task_lock(selected_task->id);
    event_log.append(
        runtime.project_name,
        EventRecord{
            std::nullopt,
            execution.run_id,
            "lock.released",
            "ap.start",
            {EventPayloadField{"lock_type", json_string("project")}},
        });
    lock_manager.release_project_lock();

    if (execution.status == StartExecutionStatus::Completed) {
      std::cout << "completed task: " << execution.task_title << '\n';
      return 0;
    }
    if (execution.status == StartExecutionStatus::Blocked) {
      std::cerr << "ap start blocked: "
                << execution.blocker_reason.value_or("blocked by external dependency") << '\n';
      return 1;
    }
    if (execution.status == StartExecutionStatus::TimedOut) {
      std::cerr << "ap start failed: task timed out after " << timeout_seconds << " seconds\n";
      return 1;
    }
    if (execution.status == StartExecutionStatus::Rework) {
      std::cerr << "ap start: reviewer returned rework for task " << execution.task_id
                << " (cycle " << execution.review_cycle_count << "/"
                << execution.max_review_cycles << ")\n";
      return 1;
    }

    std::cerr << "ap start failed: agent exited with status " << execution.agent_exit_code << '\n';
    return 1;
  } catch (const std::exception& e) {
    const std::string message = e.what();
    if (message == "Please run ap init first" || message == "project not found" ||
        message == "invalid project name") {
      std::cerr << message << '\n';
    } else if (message == "no managed path" ||
               message == "project has multiple paths and no 'main'") {
      std::cerr << "ap start failed: " << message << '\n';
    } else {
      std::cerr << "ap start failed: " << message << '\n';
    }
    return 1;
  }
}
