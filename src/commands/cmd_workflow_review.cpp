#include "autopilot/commands/cmd_workflow.hpp"

#include "autopilot/agents/agent_launcher.hpp"
#include "autopilot/projects/todo_task_selector.hpp"
#include "autopilot/runtime/alert_store.hpp"
#include "autopilot/runtime/json_utils.hpp"
#include "autopilot/runtime/lock_manager.hpp"
#include "autopilot/runtime/workflow.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int cmd_review_start(
    const std::string& coder_run_id, const std::optional<std::string>& maybe_actor_name) {
  try {
    const std::optional<RunLocator> coder_run = locate_run_by_id(coder_run_id);
    if (!coder_run.has_value()) {
      throw std::runtime_error("coder run not found");
    }
    if (coder_run->meta.role != "coder") {
      throw std::runtime_error("run is not a coder run");
    }
    if (coder_run->meta.exit_reason.value_or("") != "done") {
      throw std::runtime_error("coder run is not finished with done");
    }

    SyncedContext synced = load_synced_context(coder_run->runtime.project_name, "ap.review.start");
    EventLog event_log(synced.runtime.events_file);
    TaskState* task = find_task_by_id(synced.sync_result.tasks, coder_run->meta.task_id);
    if (task == nullptr) {
      throw std::runtime_error("task not found");
    }
    if (task->status != "review_pending" && task->status != "done") {
      throw std::runtime_error("task is not ready for review");
    }

    std::string coder_actor_name = coder_run->meta.agent;
    if (coder_actor_name.rfind("coder.", 0) == 0) {
      coder_actor_name = coder_actor_name.substr(6);
    }
    const std::string actor_name = maybe_actor_name.value_or(
        resolve_reviewer_agent_name(std::nullopt, coder_actor_name, true));
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
            task->id, expected_run_id, synced.runtime.default_timeout_seconds)) {
      lock_manager.release_project_lock();
      throw std::runtime_error("task is already locked");
    }

    const fs::path coder_stdout_file = coder_run->run_dir / "stdout.log";
    const std::string coder_stdout = fs::exists(coder_stdout_file)
                                         ? read_text_file(coder_stdout_file)
                                         : std::string();
    PublishedRunInfo published = publish_reviewer_run(
        synced.runtime,
        event_log,
        synced.sync_result.tasks,
        *task,
        synced.runtime.run_counter,
        reviewer_logical_agent_name(actor_name),
        coder_run_id,
        coder_run->meta.exit_reason.value_or("done"),
        coder_stdout,
        "published",
        "ap.review.start",
        "review.published");

    lock_manager.release_task_lock(task->id);
    lock_manager.release_project_lock();

    std::cout << "run_id=" << published.run_id << '\n';
    std::cout << "task_id=" << task->id << '\n';
    std::cout << "actor=" << actor_name << '\n';
    std::cout << "prompt_file=" << published.prompt_file.string() << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap review start failed: " << e.what() << '\n';
    return 1;
  }
}

int cmd_review_submit(
    const std::string& reviewer_run_id,
    const std::string& verdict_name,
    const std::optional<std::string>& maybe_summary,
    const std::vector<std::string>& issues,
    const std::vector<std::string>& suggestions,
    const std::optional<std::string>& maybe_reason,
    const std::optional<std::string>& maybe_category) {
  try {
    const std::optional<RunLocator> reviewer_run = locate_run_by_id(reviewer_run_id);
    if (!reviewer_run.has_value()) {
      throw std::runtime_error("reviewer run not found");
    }
    if (reviewer_run->meta.role != "reviewer") {
      throw std::runtime_error("run is not a reviewer run");
    }
    if (reviewer_run->meta.status == "finished") {
      throw std::runtime_error("reviewer run is already finished");
    }

    SyncedContext synced = load_synced_context(reviewer_run->runtime.project_name, "ap.review.submit");
    EventLog event_log(synced.runtime.events_file);
    TaskState* task = find_task_by_id(synced.sync_result.tasks, reviewer_run->meta.task_id);
    if (task == nullptr) {
      throw std::runtime_error("task not found");
    }

    const ReviewerVerdict reviewer_verdict = build_cli_reviewer_verdict(
        verdict_name, maybe_summary, issues, suggestions, maybe_reason, maybe_category);
    const std::string reviewer_ended_at = current_timestamp_with_offset();
    const int review_cycle = reviewer_run->meta.review_cycle.value_or(task->review_cycle_count + 1);
    const std::string pre_final_status = task->status;
    std::optional<AlertRecord> created_alert;
    bool todo_update_applied = false;
    std::string reviewer_summary = reviewer_verdict.summary.value_or(reviewer_verdict.verdict);

    if (reviewer_verdict.verdict == "approve") {
      task->status = "done";
      task->review_result = "approve";
      task->last_error = std::nullopt;
      task->blocker_reason = std::nullopt;
      task->blocker_category = std::nullopt;
      task->review_feedback = std::nullopt;
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
      event_log.append(
          synced.runtime.project_name,
          EventRecord{
              task->id,
              reviewer_run_id,
              "review.approved",
              "ap.review.submit",
              {
                  EventPayloadField{"reviewer_run_id", json_string(reviewer_run_id)},
                  EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                  EventPayloadField{
                      "summary",
                      json_string(reviewer_verdict.summary.value_or("approved")),
                  },
              },
          });
    } else if (reviewer_verdict.verdict == "rework") {
      ++task->review_cycle_count;
      task->review_result = "rework";
      task->review_feedback = TaskState::ReviewFeedback{
          task->review_cycle_count,
          reviewer_run_id,
          reviewer_verdict.issues,
          reviewer_verdict.suggestions,
          reviewer_ended_at,
      };
      if (task->review_cycle_count > effective_max_review_cycles(*task, synced.runtime.previous_project_state.value_or(ProjectState{}))) {
        const int max_review_cycles = effective_max_review_cycles(
            *task, synced.runtime.previous_project_state.value_or(ProjectState{}));
        const std::string reason =
            "max review cycles exceeded (" + std::to_string(task->review_cycle_count) + "/" +
            std::to_string(max_review_cycles) + ")";
        task->status = "blocked";
        task->last_error = reason;
        task->blocker_reason = reason;
        task->blocker_category = "review_cycle_limit";
        reviewer_summary = reason;
        created_alert = AlertStore(synced.runtime.alerts_dir).create(
            synced.runtime.project_name,
            task->id,
            reviewer_run_id,
            AlertDraft{"medium", "review_cycle_exceeded", reason},
            reviewer_ended_at);
        event_log.append(
            synced.runtime.project_name,
            EventRecord{
                task->id,
                reviewer_run_id,
                "task.review_cycle_exceeded",
                "ap.review.submit",
                {
                    EventPayloadField{"reviewer_run_id", json_string(reviewer_run_id)},
                    EventPayloadField{
                        "review_cycle_count",
                        std::to_string(task->review_cycle_count),
                    },
                    EventPayloadField{
                        "max_review_cycles",
                        std::to_string(max_review_cycles),
                    },
                },
            });
      } else {
        task->status = "todo";
        task->last_run_exit_reason = std::nullopt;
        task->last_error = std::nullopt;
        task->blocker_reason = std::nullopt;
        task->blocker_category = std::nullopt;
        event_log.append(
            synced.runtime.project_name,
            EventRecord{
                task->id,
                reviewer_run_id,
                "review.rework_requested",
                "ap.review.submit",
                {
                    EventPayloadField{"reviewer_run_id", json_string(reviewer_run_id)},
                    EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                    EventPayloadField{"issues", json_string_array(reviewer_verdict.issues)},
                    EventPayloadField{
                        "suggestions",
                        json_string_array(reviewer_verdict.suggestions),
                    },
                },
            });
      }
    } else {
      task->status = "blocked";
      task->review_result = "blocked";
      task->last_error = reviewer_verdict.reason;
      task->blocker_reason = reviewer_verdict.reason;
      task->blocker_category = reviewer_verdict.category.value_or("other");
      reviewer_summary = reviewer_verdict.reason.value_or("blocked");
      created_alert = AlertStore(synced.runtime.alerts_dir).create(
          synced.runtime.project_name,
          task->id,
          reviewer_run_id,
          AlertDraft{
              "high",
              "reviewer_blocked",
              "reviewer blocked " + task->id + ": " + reviewer_verdict.reason.value_or("blocked"),
          },
          reviewer_ended_at);
      event_log.append(
          synced.runtime.project_name,
          EventRecord{
              task->id,
              reviewer_run_id,
              "review.blocked",
              "ap.review.submit",
              {
                  EventPayloadField{"reviewer_run_id", json_string(reviewer_run_id)},
                  EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                  EventPayloadField{
                      "reason",
                      json_string(reviewer_verdict.reason.value_or("blocked")),
                  },
                  EventPayloadField{
                      "category",
                      json_string(reviewer_verdict.category.value_or("other")),
                  },
              },
          });
    }

    task->last_run_exit_reason = "done";
    task->updated_at = reviewer_ended_at;
    save_task_state(synced.runtime.tasks_dir, *task);

    const ProjectState project_state = build_project_state(
        synced.runtime.project_name,
        synced.sync_result.tasks,
        synced.runtime.previous_project_state,
        std::nullopt,
        std::nullopt,
        reviewer_run_id,
        reviewer_ended_at,
        synced.runtime.run_counter,
        synced.runtime.default_timeout_seconds,
        reviewer_ended_at);
    save_project_state(synced.runtime.project_state_file, project_state);

    write_text_file(
        reviewer_run->meta_file,
        build_meta_json(build_run_meta_data(
            reviewer_run_id,
            synced.runtime.project_name,
            *task,
            synced.runtime.selected_path,
            reviewer_run->meta.agent,
            "reviewer",
            task->attempt_count,
            "finished",
            reviewer_run->meta.started_at,
            reviewer_ended_at,
            0,
            "done",
            reviewer_run->meta.coder_run_id,
            review_cycle,
            reviewer_verdict.verdict)));

    const std::string reviewer_result_json = build_result_json(
        reviewer_run_id,
        *task,
        task->attempt_count,
        "succeeded",
        0,
        reviewer_run->meta.started_at,
        reviewer_ended_at,
        0,
        todo_update_applied,
        task->status,
        reviewer_summary,
        task->blocker_reason,
        created_alert.has_value() ? std::optional<std::string>(created_alert->id) : std::nullopt);
    write_text_file(reviewer_run->run_dir / "result.json", reviewer_result_json);
    write_text_file(synced.runtime.last_run_file, reviewer_result_json);

    event_log.append(
        synced.runtime.project_name,
        EventRecord{
            task->id,
            reviewer_run_id,
            "run.finished",
            "ap.review.submit",
            {
                EventPayloadField{"agent", json_string(reviewer_run->meta.agent)},
                EventPayloadField{"role", json_string("reviewer")},
                EventPayloadField{"exit_code", "0"},
            },
        });
    if (created_alert.has_value()) {
      event_log.append(
          synced.runtime.project_name,
          EventRecord{
              task->id,
              reviewer_run_id,
              "alert.created",
              "ap.review.submit",
              {
                  EventPayloadField{"alert_id", json_string(created_alert->id)},
                  EventPayloadField{"severity", json_string(created_alert->severity)},
                  EventPayloadField{"type", json_string(created_alert->type)},
                  EventPayloadField{"message", json_string(created_alert->message)},
              },
          });
    }
    append_status_changed_event(
        event_log,
        synced.runtime.project_name,
        *task,
        reviewer_run_id,
        pre_final_status,
        task->status,
        "review_finalized",
        "ap.review.submit");
    event_log.append(
        synced.runtime.project_name,
        EventRecord{
            task->id,
            reviewer_run_id,
            "result.final",
            "ap.review.submit",
            {
                EventPayloadField{"final_task_status", json_string(task->status)},
                EventPayloadField{"process_exit_code", "0"},
                EventPayloadField{"process_status", json_string("succeeded")},
                EventPayloadField{"summary", json_string(reviewer_summary)},
            },
        });

    std::cout << "run_id=" << reviewer_run_id << '\n';
    std::cout << "task_id=" << task->id << '\n';
    std::cout << "task_status=" << task->status << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap review submit failed: " << e.what() << '\n';
    return 1;
  }
}
