#include "autopilot/runtime/run_result_classifier.hpp"

#include <array>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace {

struct MarkerSpec {
  const char* marker;
  const char* category;
  bool approval_required;
  const char* alert_type;
  const char* severity;
};

struct BlockerMatch {
  std::string reason;
  std::string category;
  bool approval_required;
  std::optional<AlertDraft> alert;
};

const std::array<MarkerSpec, 5> kMarkerSpecs{{
    {"AUTOPILOT_APPROVAL_REQUIRED:", "approval_required", true, "approval_required", "high"},
    {"AUTOPILOT_HUMAN_DECISION_REQUIRED:",
     "human_decision_required",
     false,
     "human_decision_required",
     "high"},
    {"AUTOPILOT_CREDENTIAL_REQUIRED:", "credential_required", false, "credential_required", "high"},
    {"AUTOPILOT_DANGEROUS_ACTION_REQUESTED:",
     "dangerous_action_requested",
     false,
     "dangerous_action_requested",
     "high"},
    {"AUTOPILOT_BLOCKED:", "blocked", false, nullptr, nullptr},
}};

std::string trim_ascii_whitespace(const std::string& s) {
  const std::size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::string strip_known_agent_prefix(const std::string& line) {
  for (const std::string prefix : {"claude:", "codex:"}) {
    if (line.rfind(prefix, 0) == 0) {
      return trim_ascii_whitespace(line.substr(prefix.size()));
    }
  }
  return line;
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to classify run result");
  }

  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

std::string read_last_non_empty_line(const std::filesystem::path& file) {
  std::ifstream in(file);
  if (!in) {
    return "";
  }

  std::string line;
  std::string last_non_empty;
  while (std::getline(in, line)) {
    const std::string trimmed = trim_ascii_whitespace(line);
    if (!trimmed.empty()) {
      last_non_empty = trimmed;
    }
  }
  return last_non_empty;
}

std::optional<BlockerMatch> find_blocker_marker(const std::string& contents) {
  std::istringstream input(contents);
  std::string line;
  while (std::getline(input, line)) {
    const std::string trimmed_line = strip_known_agent_prefix(trim_ascii_whitespace(line));
    for (const MarkerSpec& spec : kMarkerSpecs) {
      const std::string marker(spec.marker);
      if (trimmed_line.rfind(marker, 0) != 0) {
        continue;
      }

      BlockerMatch match;
      match.reason = trim_ascii_whitespace(trimmed_line.substr(marker.size()));
      if (match.reason.empty()) {
        match.reason = trimmed_line;
      }
      match.category = spec.category;
      match.approval_required = spec.approval_required;
      if (spec.alert_type != nullptr && spec.severity != nullptr) {
        match.alert = AlertDraft{spec.severity, spec.alert_type, match.reason};
      }
      return match;
    }
  }
  return std::nullopt;
}

} // namespace

RunResultClassification classify_run_result(
    const int exit_code, const std::filesystem::path& stdout_log, const std::filesystem::path& stderr_log) {
  const std::string stdout_contents = read_text_file(stdout_log);
  const std::string stderr_contents = read_text_file(stderr_log);
  const std::optional<BlockerMatch> blocker =
      find_blocker_marker(stdout_contents).has_value() ? find_blocker_marker(stdout_contents)
                                                       : find_blocker_marker(stderr_contents);

  RunResultClassification result;
  result.process_status = exit_code == 0 ? "succeeded" : "failed";
  result.summary_excerpt = read_last_non_empty_line(stdout_log);
  if (result.summary_excerpt.empty()) {
    result.summary_excerpt = read_last_non_empty_line(stderr_log);
  }

  if (blocker.has_value()) {
    result.final_task_status = "blocked";
    result.blocker_reason = blocker->reason;
    result.blocker_category = blocker->category;
    result.approval_required = blocker->approval_required;
    result.alert = blocker->alert;
    if (result.summary_excerpt.empty()) {
      result.summary_excerpt = blocker->reason;
    }
    return result;
  }

  result.final_task_status = exit_code == 0 ? "done" : "failed";
  return result;
}
