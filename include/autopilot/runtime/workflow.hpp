#pragma once

#include "autopilot/agents/agent_launcher.hpp"
#include "autopilot/projects/project_store.hpp"
#include "autopilot/projects/todo_task_sync.hpp"
#include "autopilot/runtime/event_log.hpp"
#include "autopilot/runtime/lock_manager.hpp"
#include "autopilot/runtime/task_state_store.hpp"

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <set>
#include <string>
#include <sys/types.h>
#include <vector>

struct SelectedProjectPath {
  std::string name;
  std::filesystem::path path;
};

struct RunMetaData {
  std::string run_id;
  std::string task_id;
  int attempt_number = 0;
  std::string project;
  std::string task_title;
  std::string task_source_file;
  std::size_t task_source_line = 0;
  std::string task_original_line;
  std::string path_name;
  std::string working_directory;
  std::string agent;
  std::string role = "coder";
  std::string status;
  std::string started_at;
  std::optional<std::string> ended_at;
  std::optional<int> exit_code;
  std::optional<std::string> exit_reason;
  std::optional<std::string> coder_run_id;
  std::optional<int> review_cycle;
  std::optional<std::string> verdict;
};

struct ReviewerVerdict {
  std::string verdict;
  std::optional<std::string> summary;
  std::vector<std::string> issues;
  std::vector<std::string> suggestions;
  std::optional<std::string> reason;
  std::optional<std::string> category;
};

struct RuntimeContext {
  std::string project_name;
  std::filesystem::path project_dir;
  std::filesystem::path projects_file;
  std::filesystem::path todo_file;
  std::filesystem::path runtime_dir;
  std::filesystem::path runs_dir;
  std::filesystem::path events_file;
  std::filesystem::path state_dir;
  std::filesystem::path project_state_file;
  std::filesystem::path tasks_dir;
  std::filesystem::path lock_dir;
  std::filesystem::path alerts_dir;
  std::filesystem::path last_run_file;
  ProjectConfig project_config;
  SelectedProjectPath selected_path;
  std::optional<ProjectState> previous_project_state;
  int run_counter = 0;
  int default_timeout_seconds = 1800;
};

struct SyncedContext {
  RuntimeContext runtime;
  std::vector<TaskState> existing_tasks;
  TodoSyncResult sync_result;
  std::string synced_at;
};

struct RunLocator {
  RuntimeContext runtime;
  RunMetaData meta;
  std::filesystem::path run_dir;
  std::filesystem::path meta_file;
};

struct PublishedRunInfo {
  std::string run_id;
  std::filesystem::path run_dir;
  std::filesystem::path meta_file;
  std::filesystem::path prompt_file;
  std::filesystem::path stdout_file;
  std::filesystem::path stderr_file;
  std::filesystem::path result_file;
  std::filesystem::path timeout_marker_file;
  std::string started_at;
  std::string previous_status;
  std::string prompt;
};

enum class StartExecutionStatus {
  Completed,
  Blocked,
  TimedOut,
  Failed,
  Rework,
};

struct StartExecutionResult {
  std::string run_id;
  std::string task_id;
  std::string task_title;
  StartExecutionStatus status = StartExecutionStatus::Failed;
  std::optional<std::string> blocker_reason;
  int agent_exit_code = 1;
  int review_cycle_count = 0;
  int max_review_cycles = 0;
};

std::string shell_quote(const std::string& s);
std::string trim_ascii_whitespace(const std::string& s);
std::string strip_known_agent_prefix(const std::string& line);
bool nullable_integer_field_is_null(const std::string& json, const std::string& key);
std::string current_timestamp_with_offset();
std::string format_run_id_with_counter(int run_counter);
std::string allocate_run_id_with_counter(
    const std::filesystem::path& runs_dir,
    int run_counter);
std::string build_prompt(
    const std::string& project_name,
    const SelectedProjectPath& selected_path,
    const TaskState& task);
std::string build_reviewer_prompt(
    const TaskState& task,
    const std::string& coder_run_id,
    const std::string& coder_exit_reason,
    const std::string& coder_stdout);
std::string read_text_file(const std::filesystem::path& path);
void write_text_file(const std::filesystem::path& path, const std::string& content);
std::vector<std::string> extract_json_objects(const std::string& text);
std::optional<ReviewerVerdict> parse_reviewer_verdict(const std::string& normalized_output);
SelectedProjectPath resolve_project_path(
    const std::filesystem::path& projects_file,
    const std::string& project_name);
std::string sanitize_tmux_name(std::string s);
bool tmux_is_available();
void touch_empty_file(const std::filesystem::path& path);
int read_exit_code_file(const std::filesystem::path& exit_code_file);
int decode_system_exit_code(int system_status);
pid_t launch_bash_command_async(const std::string& command);
int wait_for_process(pid_t pid);
std::optional<int> wait_for_pid_file(
    const std::filesystem::path& pid_file,
    int attempts = 500);
std::string build_logged_agent_command(
    const std::string& agent_name,
    const std::string& prompt,
    const std::filesystem::path& working_directory,
    const std::string& stdout_redirection,
    const std::string& stderr_redirection);
std::string build_timed_shell_fragment(
    const std::string& command,
    const std::filesystem::path& timeout_marker_file,
    int timeout_seconds);
AgentLaunchResult run_agent_in_tmux(
    const std::string& project_name,
    const std::string& run_id,
    const std::string& agent_name,
    const std::string& prompt,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& stdout_log,
    const std::filesystem::path& stderr_log,
    int timeout_seconds,
    const std::function<void(int)>& on_runner_started);
AgentLaunchResult run_agent_with_timeout(
    const std::string& agent_name,
    const std::string& prompt,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& stdout_log,
    const std::filesystem::path& stderr_log,
    const std::filesystem::path& timeout_marker_file,
    int timeout_seconds);
std::string resolve_project_name(const std::optional<std::string>& maybe_project_name);
std::string build_meta_json(const RunMetaData& meta);
RunMetaData build_run_meta_data(
    const std::string& run_id,
    const std::string& project_name,
    const TaskState& task,
    const SelectedProjectPath& selected_path,
    const std::string& logical_agent_name,
    const std::string& role,
    int attempt_number,
    const std::string& status,
    const std::string& started_at,
    const std::optional<std::string>& ended_at,
    const std::optional<int>& exit_code,
    const std::optional<std::string>& exit_reason,
    const std::optional<std::string>& coder_run_id = std::nullopt,
    const std::optional<int>& review_cycle = std::nullopt,
    const std::optional<std::string>& verdict = std::nullopt);
std::optional<RunMetaData> parse_run_meta_file(const std::filesystem::path& meta_file);
std::string build_result_json(
    const std::string& run_id,
    const TaskState& task,
    int attempt_number,
    const std::string& process_status,
    int process_exit_code,
    const std::string& started_at,
    const std::string& ended_at,
    long long duration_ms,
    bool todo_update_applied,
    const std::string& final_task_status,
    const std::string& summary_excerpt,
    const std::optional<std::string>& blocker_reason,
    const std::optional<std::string>& alert_id);
void finalize_interrupted_reviewer_run(
    const std::filesystem::path& runs_dir,
    const RunMetaData& meta,
    const std::string& ended_at);
std::vector<TaskStatusChange> recover_stale_tasks(
    std::vector<TaskState>& tasks,
    const std::filesystem::path& runs_dir,
    const std::string& recovered_at);
TaskState* find_task_by_id(std::vector<TaskState>& tasks, const std::string& task_id);
void append_task_updated_event(
    EventLog& event_log,
    const std::string& project_name,
    const TaskUpdateChange& change,
    const std::string& actor = "ap.start");
void validate_task_state(
    const std::vector<TaskState>& tasks,
    const std::set<std::string>& valid_path_names);
bool status_is_runnable_candidate(const TaskState& task);
bool is_retry_exhausted(const TaskState& task);
bool task_matches_selected_path(const TaskState& task, const std::string& selected_path_name);
std::string coder_logical_agent_name(const std::string& agent_name);
std::string reviewer_logical_agent_name(const std::string& agent_name);
bool review_flow_enabled_for_task(
    const TaskState& task,
    const ProjectState& project_state,
    const std::optional<bool>& maybe_review_enabled);
int effective_max_review_cycles(const TaskState& task, const ProjectState& project_state);
std::string reviewer_failure_exit_reason(int exit_code);
TaskState* select_runnable_task(
    std::vector<TaskState>& tasks,
    const std::string& selected_path_name,
    int& retry_exhausted_count);
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
    const std::string& updated_at);
void rollback_prelaunch_task_update(
    const std::filesystem::path& tasks_dir,
    const std::filesystem::path& project_state_file,
    const std::string& project_name,
    std::vector<TaskState>& tasks,
    TaskState& task,
    const TaskState& previous_task,
    const std::optional<ProjectState>& previous_project_state,
    int run_counter,
    int default_timeout_seconds,
    const std::string& rollback_at);
void append_status_changed_event(
    EventLog& event_log,
    const std::string& project_name,
    const TaskState& task,
    const std::optional<std::string>& run_id,
    const std::string& from_status,
    const std::string& to_status,
    const std::string& reason,
    const std::string& actor = "ap.start");
void finalize_postrun_failure(
    const std::filesystem::path& tasks_dir,
    const std::filesystem::path& project_state_file,
    const std::string& project_name,
    std::vector<TaskState>& tasks,
    TaskState& task,
    const std::optional<ProjectState>& previous_project_state,
    const std::string& run_id,
    int run_counter,
    int default_timeout_seconds,
    const std::string& failure_reason,
    const std::string& failed_at);
std::string completed_todo_line(const std::string& line);
bool is_supported_actor_name(const std::string& actor_name);
RuntimeContext load_runtime_context(const std::optional<std::string>& maybe_project_name);
SyncedContext load_synced_context(
    const std::optional<std::string>& maybe_project_name,
    const std::string& actor);
ProjectState persist_synced_state(
    const RuntimeContext& runtime,
    EventLog& event_log,
    TodoSyncResult& sync_result,
    const std::vector<TaskStatusChange>& recovered_in_progress_changes,
    int run_counter,
    const std::string& updated_at,
    const std::string& actor);
void append_retry_exhausted_events(
    EventLog& event_log,
    const std::string& project_name,
    const std::vector<TaskState>& tasks,
    const std::string& actor,
    std::ostream& diagnostics);
std::optional<RunLocator> locate_run_by_id(const std::string& run_id);
std::string normalize_final_status(const std::string& status);
std::string normalize_task_status_override(const std::string& status);
ReviewerVerdict build_cli_reviewer_verdict(
    const std::string& verdict_name,
    const std::optional<std::string>& maybe_summary,
    const std::vector<std::string>& issues,
    const std::vector<std::string>& suggestions,
    const std::optional<std::string>& maybe_reason,
    const std::optional<std::string>& maybe_category);
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
    const std::string& run_event_reason);
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
    const std::string& run_event_type);
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
    const std::optional<bool>& maybe_review_enabled);
StartExecutionResult execute_ap_start_review_phase(
    RuntimeContext& runtime,
    EventLog& event_log,
    std::vector<TaskState>& tasks,
    TaskState& selected_task,
    int& run_counter,
    const ProjectState& project_state,
    int timeout_seconds,
    const PublishedRunInfo& coder_run,
    const std::string& coder_exit_reason,
    const std::string& reviewer_agent_name);
