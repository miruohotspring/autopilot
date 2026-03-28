#include "autopilot/runtime/workflow.hpp"

#include "autopilot/projects/todo_task_selector.hpp"
#include "autopilot/runtime/alert_store.hpp"
#include "autopilot/runtime/json_utils.hpp"
#include "autopilot/runtime/run_result_classifier.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

StartExecutionResult execute_ap_start_review_phase(
    RuntimeContext& runtime,
    EventLog& event_log,
    std::vector<TaskState>& tasks,
    TaskState& selected_task,
    int& run_counter,
    const ProjectState& initial_project_state,
    const int timeout_seconds,
    const PublishedRunInfo& coder_run,
    const std::string& coder_exit_reason,
    const std::string& reviewer_agent_name) {
  StartExecutionResult result;
  result.task_id = selected_task.id;
  result.task_title = selected_task.title;

  const std::string reviewer_agent = reviewer_logical_agent_name(reviewer_agent_name);
  ProjectState project_state = initial_project_state;
  std::optional<AlertRecord> created_alert;
  std::optional<std::string> blocker_reason;
  bool succeeded = false;
  bool blocked = false;
  bool todo_update_applied = false;

  PublishedRunInfo published_review = publish_reviewer_run(
      runtime,
      event_log,
      tasks,
      selected_task,
      run_counter,
      reviewer_agent,
      coder_run.run_id,
      coder_exit_reason,
      read_text_file(coder_run.stdout_file),
      "starting",
      "ap.start",
      "review.started");
  const int review_cycle = selected_task.review_cycle_count + 1;

  write_text_file(
      published_review.meta_file,
      build_meta_json(build_run_meta_data(
          published_review.run_id,
          runtime.project_name,
          selected_task,
          runtime.selected_path,
          reviewer_agent,
          "reviewer",
          selected_task.attempt_count,
          "running",
          published_review.started_at,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          coder_run.run_id,
          review_cycle)));

  AgentLaunchResult reviewer_launch_result{reviewer_agent_name, 1};
  std::chrono::steady_clock::time_point reviewer_started_clock;
  try {
    reviewer_started_clock = std::chrono::steady_clock::now();
    reviewer_launch_result = run_agent_with_timeout(
        reviewer_agent_name,
        published_review.prompt,
        runtime.selected_path.path,
        published_review.stdout_file,
        published_review.stderr_file,
        published_review.timeout_marker_file,
        timeout_seconds);
  } catch (const std::exception& reviewer_launch_error) {
    const std::string reviewer_ended_at = current_timestamp_with_offset();
    const std::string reviewer_exit_reason = "spawn_failed";
    const std::string reason =
        "reviewer failed to start: " + std::string(reviewer_launch_error.what());
    selected_task.status = "blocked";
    selected_task.last_run_exit_reason = reviewer_exit_reason;
    selected_task.last_error = reason;
    selected_task.blocker_reason = reason;
    selected_task.blocker_category = "reviewer_error";
    selected_task.review_result = "reviewer_error";
    selected_task.updated_at = reviewer_ended_at;
    save_task_state(runtime.tasks_dir, selected_task);

    project_state = build_project_state(
        runtime.project_name,
        tasks,
        runtime.previous_project_state,
        std::nullopt,
        std::nullopt,
        published_review.run_id,
        reviewer_ended_at,
        run_counter,
        runtime.default_timeout_seconds,
        reviewer_ended_at);
    save_project_state(runtime.project_state_file, project_state);

    write_text_file(
        published_review.meta_file,
        build_meta_json(build_run_meta_data(
            published_review.run_id,
            runtime.project_name,
            selected_task,
            runtime.selected_path,
            reviewer_agent,
            "reviewer",
            selected_task.attempt_count,
            "finished",
            published_review.started_at,
            reviewer_ended_at,
            reviewer_launch_result.exit_code,
            reviewer_exit_reason,
            coder_run.run_id,
            review_cycle,
            "blocked")));
    write_text_file(
        published_review.result_file,
        build_result_json(
            published_review.run_id,
            selected_task,
            selected_task.attempt_count,
            "failed",
            reviewer_launch_result.exit_code,
            published_review.started_at,
            reviewer_ended_at,
            0,
            false,
            selected_task.status,
            reason,
            selected_task.blocker_reason,
            std::nullopt));
    created_alert = AlertStore(runtime.alerts_dir).create(
        runtime.project_name,
        selected_task.id,
        published_review.run_id,
        AlertDraft{"medium", "reviewer_error", reason},
        reviewer_ended_at);
    write_text_file(
        runtime.last_run_file,
        build_result_json(
            published_review.run_id,
            selected_task,
            selected_task.attempt_count,
            "failed",
            reviewer_launch_result.exit_code,
            published_review.started_at,
            reviewer_ended_at,
            0,
            false,
            selected_task.status,
            reason,
            selected_task.blocker_reason,
            created_alert.has_value() ? std::optional<std::string>(created_alert->id)
                                      : std::nullopt));
    event_log.append(
        runtime.project_name,
        EventRecord{
            selected_task.id,
            published_review.run_id,
            "review.blocked",
            reviewer_agent,
            {
                EventPayloadField{"task_id", json_string(selected_task.id)},
                EventPayloadField{"reviewer_run_id", json_string(published_review.run_id)},
                EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                EventPayloadField{"reason", json_string(reason)},
                EventPayloadField{"category", json_string("reviewer_error")},
            },
        });
    event_log.append(
        runtime.project_name,
        EventRecord{
            selected_task.id,
            published_review.run_id,
            "alert.created",
            "ap.start",
            {
                EventPayloadField{"alert_id", json_string(created_alert->id)},
                EventPayloadField{"severity", json_string(created_alert->severity)},
                EventPayloadField{"type", json_string(created_alert->type)},
                EventPayloadField{"message", json_string(created_alert->message)},
            },
        });
    blocked = true;
    blocker_reason = reason;
  }

  if (!blocked) {
    const auto reviewer_ended_clock = std::chrono::steady_clock::now();
    const std::string reviewer_ended_at = current_timestamp_with_offset();
    const long long reviewer_duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            reviewer_ended_clock - reviewer_started_clock)
            .count();
    std::string reviewer_exit_reason;
    std::string reviewer_process_status =
        reviewer_launch_result.exit_code == 0 ? "succeeded" : "failed";
    std::optional<ReviewerVerdict> reviewer_verdict;

    if (fs::exists(published_review.timeout_marker_file)) {
      fs::remove(published_review.timeout_marker_file);
      reviewer_exit_reason = "timeout";
      reviewer_process_status = "timeout";
    } else {
      rewrite_stream_json_stdout_log(
          published_review.stdout_file, published_review.stderr_file);
      if (reviewer_launch_result.exit_code == 0) {
        try {
          reviewer_verdict = parse_reviewer_verdict(strip_known_agent_prefix(
              trim_ascii_whitespace(read_text_file(published_review.stdout_file))));
        } catch (const std::exception&) {
          reviewer_exit_reason = "parse_error";
        }
        if (!reviewer_verdict.has_value() && reviewer_exit_reason.empty()) {
          reviewer_exit_reason = "parse_error";
        }
      } else {
        reviewer_exit_reason = reviewer_failure_exit_reason(reviewer_launch_result.exit_code);
      }
    }

    if (reviewer_verdict.has_value()) {
      reviewer_exit_reason = "done";
      if (reviewer_verdict->verdict == "approve") {
        selected_task.status = "done";
        selected_task.review_result = "approve";
        selected_task.last_error = std::nullopt;
        selected_task.blocker_reason = std::nullopt;
        selected_task.blocker_category = std::nullopt;
        selected_task.review_feedback = std::nullopt;
        todo_update_applied = mark_todo_task_done(
            runtime.todo_file,
            TodoTaskSelection{
                selected_task.id,
                selected_task.title,
                selected_task.source_line,
                selected_task.source_text,
                false,
            });
        if (todo_update_applied) {
          selected_task.source_text = completed_todo_line(selected_task.source_text);
        } else {
          event_log.append(
              runtime.project_name,
              EventRecord{
                  selected_task.id,
                  published_review.run_id,
                  "todo.sync_conflict",
                  "ap.start",
                  {EventPayloadField{
                      "message",
                      json_string("failed to update TODO.md after review approval"),
                  }},
              });
        }
        event_log.append(
            runtime.project_name,
            EventRecord{
                selected_task.id,
                published_review.run_id,
                "review.approved",
                reviewer_agent,
                {
                    EventPayloadField{"task_id", json_string(selected_task.id)},
                    EventPayloadField{"reviewer_run_id", json_string(published_review.run_id)},
                    EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                    EventPayloadField{
                        "summary",
                        json_string(reviewer_verdict->summary.value_or("approved")),
                    },
                },
            });
        succeeded = true;
      } else if (reviewer_verdict->verdict == "rework") {
        ++selected_task.review_cycle_count;
        selected_task.review_result = "rework";
        selected_task.review_feedback = TaskState::ReviewFeedback{
            selected_task.review_cycle_count,
            published_review.run_id,
            reviewer_verdict->issues,
            reviewer_verdict->suggestions,
            reviewer_ended_at,
        };
        if (selected_task.review_cycle_count >
            effective_max_review_cycles(selected_task, project_state)) {
          const std::string reason =
              "max review cycles exceeded (" +
              std::to_string(selected_task.review_cycle_count) + "/" +
              std::to_string(effective_max_review_cycles(selected_task, project_state)) + ")";
          selected_task.status = "blocked";
          selected_task.last_error = reason;
          selected_task.blocker_reason = reason;
          selected_task.blocker_category = "review_cycle_limit";
          created_alert = AlertStore(runtime.alerts_dir).create(
              runtime.project_name,
              selected_task.id,
              published_review.run_id,
              AlertDraft{"medium", "review_cycle_exceeded", reason},
              reviewer_ended_at);
          event_log.append(
              runtime.project_name,
              EventRecord{
                  selected_task.id,
                  published_review.run_id,
                  "task.review_cycle_exceeded",
                  reviewer_agent,
                  {
                      EventPayloadField{"task_id", json_string(selected_task.id)},
                      EventPayloadField{
                          "reviewer_run_id",
                          json_string(published_review.run_id),
                      },
                      EventPayloadField{
                          "review_cycle_count",
                          std::to_string(selected_task.review_cycle_count),
                      },
                      EventPayloadField{
                          "max_review_cycles",
                          std::to_string(
                              effective_max_review_cycles(selected_task, project_state)),
                      },
                  },
              });
          blocker_reason = reason;
          blocked = true;
        } else {
          selected_task.status = "todo";
          selected_task.last_run_exit_reason = std::nullopt;
          selected_task.last_error = std::nullopt;
          selected_task.blocker_reason = std::nullopt;
          selected_task.blocker_category = std::nullopt;
          event_log.append(
              runtime.project_name,
              EventRecord{
                  selected_task.id,
                  published_review.run_id,
                  "review.rework_requested",
                  reviewer_agent,
                  {
                      EventPayloadField{"task_id", json_string(selected_task.id)},
                      EventPayloadField{
                          "reviewer_run_id",
                          json_string(published_review.run_id),
                      },
                      EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                      EventPayloadField{
                          "issues",
                          json_string_array(reviewer_verdict->issues),
                      },
                      EventPayloadField{
                          "suggestions",
                          json_string_array(reviewer_verdict->suggestions),
                      },
                  },
              });
        }
      } else {
        selected_task.status = "blocked";
        selected_task.review_result = "blocked";
        selected_task.last_error = reviewer_verdict->reason;
        selected_task.blocker_reason = reviewer_verdict->reason;
        selected_task.blocker_category = reviewer_verdict->category.value_or("other");
        const std::string alert_message =
            "reviewer blocked " + selected_task.id + ": " +
            reviewer_verdict->reason.value_or("blocked");
        created_alert = AlertStore(runtime.alerts_dir).create(
            runtime.project_name,
            selected_task.id,
            published_review.run_id,
            AlertDraft{"high", "reviewer_blocked", alert_message},
            reviewer_ended_at);
        event_log.append(
            runtime.project_name,
            EventRecord{
                selected_task.id,
                published_review.run_id,
                "review.blocked",
                reviewer_agent,
                {
                    EventPayloadField{"task_id", json_string(selected_task.id)},
                    EventPayloadField{"reviewer_run_id", json_string(published_review.run_id)},
                    EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                    EventPayloadField{
                        "reason",
                        json_string(reviewer_verdict->reason.value_or("blocked")),
                    },
                    EventPayloadField{
                        "category",
                        json_string(reviewer_verdict->category.value_or("other")),
                    },
                    EventPayloadField{"parse_error", "false"},
                },
            });
        blocker_reason = reviewer_verdict->reason;
        blocked = true;
      }
    } else if (reviewer_exit_reason == "parse_error") {
      const std::string reason = "reviewer output could not be parsed";
      selected_task.status = "blocked";
      selected_task.review_result = "parse_error";
      selected_task.last_error = reason;
      selected_task.blocker_reason = reason;
      selected_task.blocker_category = "reviewer_error";
      created_alert = AlertStore(runtime.alerts_dir).create(
          runtime.project_name,
          selected_task.id,
          published_review.run_id,
          AlertDraft{"medium", "reviewer_parse_error", reason},
          reviewer_ended_at);
      event_log.append(
          runtime.project_name,
          EventRecord{
              selected_task.id,
              published_review.run_id,
              "review.blocked",
              reviewer_agent,
              {
                  EventPayloadField{"task_id", json_string(selected_task.id)},
                  EventPayloadField{"reviewer_run_id", json_string(published_review.run_id)},
                  EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                  EventPayloadField{"reason", json_string(reason)},
                  EventPayloadField{"category", json_string("reviewer_error")},
                  EventPayloadField{"parse_error", "true"},
              },
          });
      blocker_reason = reason;
      blocked = true;
    } else {
      const std::string reason =
          reviewer_exit_reason == "timeout"
              ? "timed out after " + std::to_string(timeout_seconds) + " seconds"
              : "reviewer exited with status " + std::to_string(reviewer_launch_result.exit_code);
      selected_task.status = "blocked";
      selected_task.review_result = "reviewer_error";
      selected_task.last_error = reason;
      selected_task.blocker_reason = reason;
      selected_task.blocker_category = "reviewer_error";
      created_alert = AlertStore(runtime.alerts_dir).create(
          runtime.project_name,
          selected_task.id,
          published_review.run_id,
          AlertDraft{"medium", "reviewer_error", reason},
          reviewer_ended_at);
      event_log.append(
          runtime.project_name,
          EventRecord{
              selected_task.id,
              published_review.run_id,
              "review.blocked",
              reviewer_agent,
              {
                  EventPayloadField{"task_id", json_string(selected_task.id)},
                  EventPayloadField{"reviewer_run_id", json_string(published_review.run_id)},
                  EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                  EventPayloadField{"reason", json_string(reason)},
                  EventPayloadField{"category", json_string("reviewer_error")},
                  EventPayloadField{"reviewer_error", "true"},
              },
          });
      blocker_reason = reason;
      blocked = true;
    }

    selected_task.last_run_exit_reason = reviewer_exit_reason;
    selected_task.updated_at = reviewer_ended_at;
    save_task_state(runtime.tasks_dir, selected_task);

    project_state = build_project_state(
        runtime.project_name,
        tasks,
        runtime.previous_project_state,
        std::nullopt,
        std::nullopt,
        published_review.run_id,
        reviewer_ended_at,
        run_counter,
        runtime.default_timeout_seconds,
        reviewer_ended_at);
    save_project_state(runtime.project_state_file, project_state);

    write_text_file(
        published_review.meta_file,
        build_meta_json(build_run_meta_data(
            published_review.run_id,
            runtime.project_name,
            selected_task,
            runtime.selected_path,
            reviewer_agent,
            "reviewer",
            selected_task.attempt_count,
            "finished",
            published_review.started_at,
            reviewer_ended_at,
            reviewer_launch_result.exit_code,
            reviewer_exit_reason,
            coder_run.run_id,
            review_cycle,
            reviewer_verdict.has_value()
                ? std::optional<std::string>(reviewer_verdict->verdict)
                : std::nullopt)));

    event_log.append_stream_file(
        runtime.project_name,
        selected_task.id,
        published_review.run_id,
        "agent." + reviewer_agent,
        "stdout",
        published_review.stdout_file);
    event_log.append_stream_file(
        runtime.project_name,
        selected_task.id,
        published_review.run_id,
        "agent." + reviewer_agent,
        "stderr",
        published_review.stderr_file);
    event_log.append(
        runtime.project_name,
        EventRecord{
            selected_task.id,
            published_review.run_id,
            "run.finished",
            "ap.start",
            {
                EventPayloadField{"agent", json_string(reviewer_agent)},
                EventPayloadField{"role", json_string("reviewer")},
                EventPayloadField{"exit_code", std::to_string(reviewer_launch_result.exit_code)},
                EventPayloadField{"duration_ms", std::to_string(reviewer_duration_ms)},
            },
        });
    if (created_alert.has_value()) {
      event_log.append(
          runtime.project_name,
          EventRecord{
              selected_task.id,
              published_review.run_id,
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

    const std::string reviewer_summary =
        reviewer_verdict.has_value()
            ? reviewer_verdict->summary.value_or(reviewer_verdict->verdict)
            : blocker_reason.value_or(reviewer_exit_reason);
    const std::string reviewer_result_json = build_result_json(
        published_review.run_id,
        selected_task,
        selected_task.attempt_count,
        reviewer_process_status,
        reviewer_launch_result.exit_code,
        published_review.started_at,
        reviewer_ended_at,
        reviewer_duration_ms,
        todo_update_applied,
        selected_task.status,
        reviewer_summary,
        selected_task.blocker_reason,
        created_alert.has_value() ? std::optional<std::string>(created_alert->id)
                                  : std::nullopt);
    write_text_file(published_review.result_file, reviewer_result_json);
    write_text_file(runtime.last_run_file, reviewer_result_json);
    append_status_changed_event(
        event_log,
        runtime.project_name,
        selected_task,
        published_review.run_id,
        "review_pending",
        selected_task.status,
        "review_finalized");
    event_log.append(
        runtime.project_name,
        EventRecord{
            selected_task.id,
            published_review.run_id,
            "result.final",
            "runtime.classifier",
            {
                EventPayloadField{"final_task_status", json_string(selected_task.status)},
                EventPayloadField{
                    "process_exit_code",
                    std::to_string(reviewer_launch_result.exit_code),
                },
                EventPayloadField{"process_status", json_string(reviewer_process_status)},
                EventPayloadField{"summary", json_string(reviewer_summary)},
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
  }

  if (succeeded) {
    result.status = StartExecutionStatus::Completed;
  } else if (blocked) {
    result.status = StartExecutionStatus::Blocked;
    result.blocker_reason = blocker_reason;
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
}
