#pragma once

#include <filesystem>
#include <optional>
#include <string>

struct AlertDraft {
  std::string severity;
  std::string type;
  std::string message;
};

struct RunResultClassification {
  std::string process_status;
  std::string final_task_status;
  std::string summary_excerpt;
  std::optional<std::string> blocker_reason;
  std::optional<std::string> blocker_category;
  bool approval_required = false;
  std::optional<AlertDraft> alert;
};

RunResultClassification classify_run_result(
    int exit_code, const std::filesystem::path& stdout_log, const std::filesystem::path& stderr_log);

// If stdout_log contains claude's stream-json output, appends the raw JSON to stderr_log
// and overwrites stdout_log with the extracted result text (with newlines preserved).
// Does nothing if stdout_log is not in stream-json format.
void rewrite_stream_json_stdout_log(
    const std::filesystem::path& stdout_log, const std::filesystem::path& stderr_log);
