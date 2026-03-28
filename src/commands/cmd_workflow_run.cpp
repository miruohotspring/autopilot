#include "autopilot/commands/cmd_workflow.hpp"

#include "autopilot/agents/agent_launcher.hpp"
#include "autopilot/projects/todo_task_selector.hpp"
#include "autopilot/runtime/json_utils.hpp"
#include "autopilot/runtime/lock_manager.hpp"
#include "autopilot/runtime/workflow.hpp"

#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

int cmd_run_start(
    const std::optional<std::string>& maybe_project_name,
    const std::optional<std::string>& maybe_task_id,
    const std::optional<std::string>& maybe_actor_name) {
  try {
    SyncedContext synced = load_synced_context(maybe_project_name, "ap.run.start");
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
        "ap.run.start");

    TaskState* selected_task = nullptr;
    int retry_exhausted_count = 0;
    if (maybe_task_id.has_value()) {
      selected_task = find_task_by_id(synced.sync_result.tasks, *maybe_task_id);
      if (selected_task == nullptr) {
        throw std::runtime_error("task not found");
      }
      if (!selected_task->present_in_todo) {
        throw std::runtime_error("task is not present in TODO.md");
      }
      if (!status_is_runnable_candidate(*selected_task)) {
        throw std::runtime_error("task is not runnable");
      }
    } else {
      selected_task = select_runnable_task(
          synced.sync_result.tasks, synced.runtime.selected_path.name, retry_exhausted_count);
    }
    if (selected_task == nullptr) {
      throw std::runtime_error("no runnable task");
    }

    const std::string actor_name = maybe_actor_name.value_or(resolve_agent_name(true));
    if (!is_supported_actor_name(actor_name)) {
      throw std::runtime_error("unsupported actor: " + actor_name);
    }

    LockManager lock_manager(synced.runtime.lock_dir);
    LockInfo stale_info{};
    bool stale_lock = false;
    const std::string probe_run_id = allocate_run_id_with_counter(synced.runtime.runs_dir, 0);
    if (!lock_manager.acquire_project_lock(
            probe_run_id, synced.runtime.default_timeout_seconds, stale_lock, stale_info)) {
      throw std::runtime_error(
          "project is already locked by pid " + std::to_string(stale_info.pid));
    }

    const int next_run_counter = synced.runtime.run_counter + 1;
    const std::string expected_run_id =
        allocate_run_id_with_counter(synced.runtime.runs_dir, next_run_counter);
    if (!lock_manager.acquire_task_lock(
            selected_task->id, expected_run_id, synced.runtime.default_timeout_seconds)) {
      lock_manager.release_project_lock();
      throw std::runtime_error("task is already locked");
    }

    PublishedRunInfo published = publish_coder_run(
        synced.runtime,
        event_log,
        synced.sync_result.tasks,
        *selected_task,
        synced.runtime.run_counter,
        coder_logical_agent_name(actor_name),
        "published",
        "ap.run.start",
        "run.published",
        "manual_run_start");

    lock_manager.release_task_lock(selected_task->id);
    lock_manager.release_project_lock();

    std::cout << "run_id=" << published.run_id << '\n';
    std::cout << "task_id=" << selected_task->id << '\n';
    std::cout << "actor=" << actor_name << '\n';
    std::cout << "prompt_file=" << published.prompt_file.string() << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap run start failed: " << e.what() << '\n';
    return 1;
  }
}

int cmd_run_finish(
    const std::string& run_id,
    const std::string& status,
    std::optional<bool> maybe_review_enabled,
    const std::optional<std::string>& maybe_summary,
    const std::optional<std::string>& maybe_blocker_reason,
    const std::optional<std::string>& maybe_blocker_category,
    const bool approval_required) {
  try {
    const std::string normalized_status = normalize_final_status(status);
    const std::optional<RunLocator> located = locate_run_by_id(run_id);
    if (!located.has_value()) {
      throw std::runtime_error("run not found");
    }
    if (located->meta.role != "coder") {
      throw std::runtime_error("run is not a coder run");
    }
    if (located->meta.status == "finished") {
      throw std::runtime_error("run is already finished");
    }

    SyncedContext synced = load_synced_context(located->runtime.project_name, "ap.run.finish");
    EventLog event_log(synced.runtime.events_file);
    TaskState* task = find_task_by_id(synced.sync_result.tasks, located->meta.task_id);
    if (task == nullptr) {
      throw std::runtime_error("task not found");
    }

    const std::string ended_at = current_timestamp_with_offset();
    const std::string pre_final_status = task->status;
    const ProjectState current_project_state = build_project_state(
        synced.runtime.project_name,
        synced.sync_result.tasks,
        synced.runtime.previous_project_state,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        synced.runtime.run_counter,
        synced.runtime.default_timeout_seconds,
        ended_at);

    const bool should_run_review =
        normalized_status == "done" &&
        review_flow_enabled_for_task(*task, current_project_state, maybe_review_enabled);
    bool todo_update_applied = false;
    std::optional<std::string> exit_reason;
    std::string process_status = "failed";
    int process_exit_code = 1;
    std::string summary = maybe_summary.value_or(normalized_status);

    if (normalized_status == "done") {
      exit_reason = "done";
      process_status = "succeeded";
      process_exit_code = 0;
      task->status = should_run_review ? "review_pending" : "done";
      task->last_error = std::nullopt;
      task->blocker_reason = std::nullopt;
      task->blocker_category = std::nullopt;
      if (!should_run_review) {
        todo_update_applied = mark_todo_task_done(
            synced.runtime.todo_file,
            TodoTaskSelection{
                task->id,
                task->title,
                task->source_line,
                task->source_text,
                false,
            });
        if (todo_update_applied) {
          task->source_text = completed_todo_line(task->source_text);
        }
      }
    } else if (normalized_status == "blocked") {
      exit_reason = "blocked";
      process_status = "failed";
      process_exit_code = 1;
      task->status = "blocked";
      task->last_error = maybe_blocker_reason.value_or("blocked");
      task->blocker_reason = maybe_blocker_reason.value_or("blocked");
      task->blocker_category = maybe_blocker_category.value_or(
          approval_required ? "approval_required" : "blocked");
      task->approval_required = approval_required;
    } else if (normalized_status == "timeout") {
      exit_reason = "timeout";
      process_status = "timeout";
      process_exit_code = 124;
      task->status = "failed";
      task->last_error = "timed out";
      task->blocker_reason = std::nullopt;
      task->blocker_category = std::nullopt;
      summary = maybe_summary.value_or("timed out");
    } else if (normalized_status == "cancelled") {
      exit_reason = "cancelled";
      process_status = "cancelled";
      process_exit_code = 1;
      task->status = "cancelled";
      task->last_error = maybe_summary;
      task->blocker_reason = std::nullopt;
      task->blocker_category = std::nullopt;
    } else {
      exit_reason = "failed";
      process_status = "failed";
      process_exit_code = 1;
      task->status = "failed";
      task->last_error = maybe_summary.value_or("manual run failed");
      task->blocker_reason = std::nullopt;
      task->blocker_category = std::nullopt;
    }

    task->last_run_exit_reason = exit_reason;
    task->updated_at = ended_at;
    save_task_state(synced.runtime.tasks_dir, *task);

    const ProjectState project_state = build_project_state(
        synced.runtime.project_name,
        synced.sync_result.tasks,
        synced.runtime.previous_project_state,
        std::nullopt,
        std::nullopt,
        run_id,
        ended_at,
        synced.runtime.run_counter,
        synced.runtime.default_timeout_seconds,
        ended_at);
    save_project_state(synced.runtime.project_state_file, project_state);

    write_text_file(
        located->meta_file,
        build_meta_json(build_run_meta_data(
            run_id,
            synced.runtime.project_name,
            *task,
            synced.runtime.selected_path,
            located->meta.agent,
            "coder",
            task->attempt_count,
            "finished",
            located->meta.started_at,
            ended_at,
            process_exit_code,
            exit_reason)));

    const std::string result_json = build_result_json(
        run_id,
        *task,
        task->attempt_count,
        process_status,
        process_exit_code,
        located->meta.started_at,
        ended_at,
        0,
        todo_update_applied,
        task->status,
        summary,
        task->blocker_reason,
        std::nullopt);
    write_text_file(located->run_dir / "result.json", result_json);
    write_text_file(synced.runtime.last_run_file, result_json);

    event_log.append(
        synced.runtime.project_name,
        EventRecord{
            task->id,
            run_id,
            "run.finished",
            "ap.run.finish",
            {
                EventPayloadField{"agent", json_string(located->meta.agent)},
                EventPayloadField{"role", json_string("coder")},
                EventPayloadField{"exit_code", std::to_string(process_exit_code)},
                EventPayloadField{"process_status", json_string(process_status)},
            },
        });
    append_status_changed_event(
        event_log,
        synced.runtime.project_name,
        *task,
        run_id,
        pre_final_status,
        task->status,
        should_run_review ? "awaiting_review" : "run_finalized",
        "ap.run.finish");
    event_log.append(
        synced.runtime.project_name,
        EventRecord{
            task->id,
            run_id,
            "result.final",
            "ap.run.finish",
            {
                EventPayloadField{"final_task_status", json_string(task->status)},
                EventPayloadField{"process_exit_code", std::to_string(process_exit_code)},
                EventPayloadField{"process_status", json_string(process_status)},
                EventPayloadField{"summary", json_string(summary)},
            },
        });

    std::cout << "run_id=" << run_id << '\n';
    std::cout << "task_id=" << task->id << '\n';
    std::cout << "task_status=" << task->status << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap run finish failed: " << e.what() << '\n';
    return 1;
  }
}
