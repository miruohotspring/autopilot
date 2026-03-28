#include "autopilot/runtime/workflow.hpp"

#include "autopilot/projects/todo_task_selector.hpp"
#include "autopilot/runtime/alert_store.hpp"
#include "autopilot/runtime/json_utils.hpp"
#include "autopilot/runtime/run_result_classifier.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

StartExecutionResult execute_ap_start_locked_run(
    RuntimeContext& runtime,
    EventLog& event_log,
    LockManager& lock_manager,
    std::vector<TaskState>& tasks,
    TaskState& selected_task,
    int& run_counter,
    int timeout_seconds,
    const std::string& agent_name,
    const std::string& reviewer_agent_name,
    const std::optional<bool>& maybe_review_enabled) {
  StartExecutionResult result;
  result.task_id = selected_task.id;
  result.task_title = selected_task.title;

  const std::string coder_agent = coder_logical_agent_name(agent_name);
  const TaskState previous_task_state = selected_task;
  AgentLaunchResult launch_result{agent_name, 1};
  bool launch_started = false;
  PublishedRunInfo published_run;
  std::chrono::steady_clock::time_point started_clock;

  try {
    published_run = publish_coder_run(
        runtime,
        event_log,
        tasks,
        selected_task,
        run_counter,
        coder_agent,
        "running",
        "ap.start",
        "run.started",
        "first_runnable_task");
    result.run_id = published_run.run_id;

    const ProjectState running_project_state = build_project_state(
        runtime.project_name,
        tasks,
        runtime.previous_project_state,
        selected_task.id,
        published_run.run_id,
        std::nullopt,
        std::nullopt,
        run_counter,
        runtime.default_timeout_seconds,
        published_run.started_at);
    const bool review_enabled_for_task =
        review_flow_enabled_for_task(selected_task, running_project_state, maybe_review_enabled);
    started_clock = std::chrono::steady_clock::now();

    if (tmux_is_available()) {
      launch_result = run_agent_in_tmux(
          runtime.project_name,
          published_run.run_id,
          agent_name,
          published_run.prompt,
          runtime.selected_path.path,
          published_run.stdout_file,
          published_run.stderr_file,
          timeout_seconds,
          [&](const int runner_pid) {
            lock_manager.transfer_project_lock_pid(runner_pid);
            lock_manager.transfer_task_lock_pid(selected_task.id, runner_pid);
            launch_started = true;
          });
    } else {
      const std::string base_cmd = build_logged_agent_command(
          agent_name,
          published_run.prompt,
          runtime.selected_path.path,
          " >" + shell_quote(published_run.stdout_file.string()),
          " 2>" + shell_quote(published_run.stderr_file.string()));
      const std::string wrapped_command =
          "set +e; " +
          build_timed_shell_fragment(base_cmd, published_run.timeout_marker_file, timeout_seconds) +
          "; exit \"$status\"";
      const pid_t runner_pid = launch_bash_command_async(wrapped_command);
      lock_manager.transfer_project_lock_pid(static_cast<int>(runner_pid));
      lock_manager.transfer_task_lock_pid(selected_task.id, static_cast<int>(runner_pid));
      launch_started = true;
      launch_result = AgentLaunchResult{agent_name, wait_for_process(runner_pid)};
    }

    lock_manager.transfer_project_lock_pid(static_cast<int>(::getpid()));
    lock_manager.transfer_task_lock_pid(selected_task.id, static_cast<int>(::getpid()));

    const auto ended_clock = std::chrono::steady_clock::now();
    const std::string ended_at = current_timestamp_with_offset();
    const long long duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     ended_clock - started_clock)
                                     .count();
    const std::string pre_final_status = selected_task.status;

    RunResultClassification classification;
    bool succeeded = false;
    bool blocked = false;
    bool timed_out = false;
    bool todo_update_applied = false;
    std::optional<AlertRecord> created_alert;
    std::string exit_reason;

    try {
      timed_out = fs::exists(published_run.timeout_marker_file);
      if (timed_out) {
        fs::remove(published_run.timeout_marker_file);
      }

      if (timed_out) {
        exit_reason = "timeout";
        event_log.append(
            runtime.project_name,
            EventRecord{
                selected_task.id,
                published_run.run_id,
                "run.timeout",
                "ap.start",
                {
                    EventPayloadField{"task_id", json_string(selected_task.id)},
                    EventPayloadField{"run_id", json_string(published_run.run_id)},
                    EventPayloadField{"timeout_seconds", std::to_string(timeout_seconds)},
                    EventPayloadField{"pid", std::to_string(static_cast<int>(::getpid()))},
                },
            });
        std::cerr << "ap start: task " << selected_task.id << " timed out after "
                  << timeout_seconds << " seconds\n";
        classification.final_task_status = "failed";
        classification.process_status = "timeout";
        classification.summary_excerpt = "timed out after " + std::to_string(timeout_seconds) + "s";
      } else {
        rewrite_stream_json_stdout_log(published_run.stdout_file, published_run.stderr_file);
        classification = classify_run_result(
            launch_result.exit_code, published_run.stdout_file, published_run.stderr_file);
      }

      const bool coder_succeeded = classification.final_task_status == "done";
      const bool coder_blocked = classification.final_task_status == "blocked";
      const bool should_run_review = coder_succeeded && review_enabled_for_task;
      succeeded = coder_succeeded && !should_run_review;
      blocked = coder_blocked;

      if (!timed_out) {
        if (coder_succeeded) {
          exit_reason = "done";
        } else if (coder_blocked) {
          exit_reason = "blocked";
        } else {
          exit_reason = "failed";
        }
      }

      selected_task.status = should_run_review ? "review_pending" : classification.final_task_status;
      selected_task.last_run_exit_reason = exit_reason;
      selected_task.updated_at = ended_at;
      if (should_run_review || classification.final_task_status == "done") {
        selected_task.last_error = std::nullopt;
        selected_task.blocker_reason = std::nullopt;
        selected_task.blocker_category = std::nullopt;
      } else if (coder_blocked) {
        selected_task.last_error = classification.blocker_reason;
        selected_task.blocker_reason = classification.blocker_reason;
        selected_task.blocker_category = classification.blocker_category;
        if (classification.approval_required) {
          selected_task.approval_required = true;
        }
      } else if (timed_out) {
        selected_task.last_error =
            std::string("timed out after ") + std::to_string(timeout_seconds) + "s";
        selected_task.blocker_reason = std::nullopt;
        selected_task.blocker_category = std::nullopt;
      } else {
        selected_task.last_error =
            std::string("agent exited with status ") + std::to_string(launch_result.exit_code);
        selected_task.blocker_reason = std::nullopt;
        selected_task.blocker_category = std::nullopt;
      }
      save_task_state(runtime.tasks_dir, selected_task);

      ProjectState project_state = build_project_state(
          runtime.project_name,
          tasks,
          runtime.previous_project_state,
          std::nullopt,
          std::nullopt,
          published_run.run_id,
          ended_at,
          run_counter,
          runtime.default_timeout_seconds,
          ended_at);
      save_project_state(runtime.project_state_file, project_state);

      if (coder_succeeded && !should_run_review) {
        todo_update_applied = mark_todo_task_done(
            runtime.todo_file,
            TodoTaskSelection{
                selected_task.id,
                selected_task.title,
                selected_task.source_line,
                selected_task.source_text,
                false,
            });
        if (!todo_update_applied) {
          event_log.append(
              runtime.project_name,
              EventRecord{
                  selected_task.id,
                  published_run.run_id,
                  "todo.sync_conflict",
                  "ap.start",
                  {EventPayloadField{
                      "message",
                      json_string("failed to update TODO.md after run"),
                  }},
              });
        } else {
          selected_task.source_text = completed_todo_line(selected_task.source_text);
          save_task_state(runtime.tasks_dir, selected_task);
        }
      }

      write_text_file(
          published_run.meta_file,
          build_meta_json(build_run_meta_data(
              published_run.run_id,
              runtime.project_name,
              selected_task,
              runtime.selected_path,
              coder_agent,
              "coder",
              selected_task.attempt_count,
              "finished",
              published_run.started_at,
              ended_at,
              launch_result.exit_code,
              exit_reason)));

      event_log.append_stream_file(
          runtime.project_name,
          selected_task.id,
          published_run.run_id,
          "agent." + coder_agent,
          "stdout",
          published_run.stdout_file);
      event_log.append_stream_file(
          runtime.project_name,
          selected_task.id,
          published_run.run_id,
          "agent." + coder_agent,
          "stderr",
          published_run.stderr_file);
      event_log.append(
          runtime.project_name,
          EventRecord{
              selected_task.id,
              published_run.run_id,
              "run.finished",
              "ap.start",
              {
                  EventPayloadField{"agent", json_string(coder_agent)},
                  EventPayloadField{"role", json_string("coder")},
                  EventPayloadField{"exit_code", std::to_string(launch_result.exit_code)},
                  EventPayloadField{"duration_ms", std::to_string(duration_ms)},
              },
          });

      if (blocked) {
        if (classification.alert.has_value()) {
          created_alert = AlertStore(runtime.alerts_dir).create(
              runtime.project_name,
              selected_task.id,
              published_run.run_id,
              *classification.alert,
              ended_at);
        }
        event_log.append(
            runtime.project_name,
            EventRecord{
                selected_task.id,
                published_run.run_id,
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
              runtime.project_name,
              EventRecord{
                  selected_task.id,
                  published_run.run_id,
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
          published_run.run_id,
          selected_task,
          selected_task.attempt_count,
          classification.process_status,
          launch_result.exit_code,
          published_run.started_at,
          ended_at,
          duration_ms,
          todo_update_applied,
          selected_task.status,
          classification.summary_excerpt,
          classification.blocker_reason,
          created_alert.has_value() ? std::optional<std::string>(created_alert->id) : std::nullopt);
      write_text_file(published_run.result_file, result_json);
      write_text_file(runtime.last_run_file, result_json);
      append_status_changed_event(
          event_log,
          runtime.project_name,
          selected_task,
          published_run.run_id,
          pre_final_status,
          selected_task.status,
          "result_finalized");
      event_log.append(
          runtime.project_name,
          EventRecord{
              selected_task.id,
              published_run.run_id,
              "result.final",
              "runtime.classifier",
              {
                  EventPayloadField{"final_task_status", json_string(selected_task.status)},
                  EventPayloadField{"process_exit_code", std::to_string(launch_result.exit_code)},
                  EventPayloadField{"process_status", json_string(classification.process_status)},
                  EventPayloadField{"summary", json_string(classification.summary_excerpt)},
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

      if (should_run_review) {
        StartExecutionResult review_result = execute_ap_start_review_phase(
            runtime,
            event_log,
            tasks,
            selected_task,
            run_counter,
            project_state,
            timeout_seconds,
            published_run,
            exit_reason,
            reviewer_agent_name);
        review_result.run_id = published_run.run_id;
        review_result.agent_exit_code = launch_result.exit_code;
        return review_result;
      }
    } catch (const std::exception& e) {
      finalize_postrun_failure(
          runtime.tasks_dir,
          runtime.project_state_file,
          runtime.project_name,
          tasks,
          selected_task,
          runtime.previous_project_state,
          published_run.run_id,
          run_counter,
          runtime.default_timeout_seconds,
          "post-run processing failed: " + std::string(e.what()),
          current_timestamp_with_offset());
      throw;
    }

    result.agent_exit_code = launch_result.exit_code;
    if (succeeded) {
      result.status = StartExecutionStatus::Completed;
    } else if (blocked) {
      result.status = StartExecutionStatus::Blocked;
      result.blocker_reason = classification.blocker_reason;
    } else if (timed_out) {
      result.status = StartExecutionStatus::TimedOut;
    } else if (selected_task.status == "todo" && selected_task.review_result == "rework") {
      result.status = StartExecutionStatus::Rework;
      result.review_cycle_count = selected_task.review_cycle_count;
      result.max_review_cycles = effective_max_review_cycles(selected_task, build_project_state(
          runtime.project_name,
          tasks,
          runtime.previous_project_state,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          run_counter,
          runtime.default_timeout_seconds,
          current_timestamp_with_offset()));
    } else {
      result.status = StartExecutionStatus::Failed;
    }
    return result;
  } catch (const std::exception&) {
    if (!launch_started && !result.run_id.empty()) {
      rollback_prelaunch_task_update(
          runtime.tasks_dir,
          runtime.project_state_file,
          runtime.project_name,
          tasks,
          selected_task,
          previous_task_state,
          runtime.previous_project_state,
          run_counter,
          runtime.default_timeout_seconds,
          current_timestamp_with_offset());
    }
    throw;
  }
}
