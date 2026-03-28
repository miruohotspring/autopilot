#include "autopilot/runtime/workflow.hpp"

#include "autopilot/commands/delete_ui.hpp"
#include "autopilot/projects/project_paths.hpp"
#include "autopilot/runtime/json_utils.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

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

bool nullable_integer_field_is_null(const std::string& json, const std::string& key) {
  const std::string pattern = "\"" + key + "\":";
  std::size_t pos = json.find(pattern);
  if (pos == std::string::npos) {
    return false;
  }
  pos = json.find_first_not_of(" \t\r\n", pos + pattern.size());
  return pos != std::string::npos && json.compare(pos, 4, "null") == 0;
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

std::string format_run_id_with_counter(const int run_counter) {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                          now.time_since_epoch()) %
                      std::chrono::seconds(1);
  std::tm local_tm{};
#if defined(_WIN32)
  localtime_s(&local_tm, &now_time);
#else
  localtime_r(&now_time, &local_tm);
#endif

  std::ostringstream oss;
  oss << "run-" << std::put_time(&local_tm, "%Y%m%d-%H%M%S")
      << std::setw(6) << std::setfill('0') << micros.count()
      << "-L"
      << std::setw(2) << std::setfill('0') << run_counter;
  return oss.str();
}

std::string allocate_run_id_with_counter(const fs::path& runs_dir, const int run_counter) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    const std::string run_id = format_run_id_with_counter(run_counter);
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
  oss << "あなたは autopilot project '" << project_name << "' の coder エージェントです。\n";
  oss << "作業ディレクトリ: " << selected_path.path.string() << "\n";
  oss << "管理パス名: " << selected_path.name << "\n";
  oss << "選択されたタスク: " << task.title << "\n";
  oss << "TODO.md 上の行番号: " << task.source_line << "\n";
  oss << "元の TODO 行: " << task.source_text << "\n";
  oss << "可能であれば選択されたタスクを実装してください。\n";
  oss << "作業ディレクトリ内のファイルは調査・編集して構いません。\n";
  oss << "思考は日本語で行い、ユーザー向けの出力も日本語で行ってください。\n";
  oss << "最後に、変更内容の要約を短く日本語で出力してください。\n";
  oss << "\n/ap-self-recognition\n";
  return oss.str();
}

std::string build_reviewer_prompt(
    const TaskState& task,
    const std::string& coder_run_id,
    const std::string& coder_exit_reason,
    const std::string& coder_stdout) {
  std::ostringstream oss;
  oss << "あなたは autopilot の reviewer エージェントです。\n";
  oss << "以下のタスクの実装結果をレビューし、verdict を JSON で返してください。\n\n";
  oss << "思考は日本語で行ってください。summary / issues / suggestions / reason も日本語で記述してください。\n\n";
  oss << "## タスク情報\n";
  oss << "- ID: " << task.id << "\n";
  oss << "- タイトル: " << task.title << "\n";
  oss << "- 説明: " << task.description.value_or("") << "\n\n";
  oss << "## coder の実行結果\n";
  oss << "- run ID: " << coder_run_id << "\n";
  oss << "- exit reason: " << coder_exit_reason << "\n\n";
  oss << "## coder の出力（stdout）\n";
  oss << coder_stdout << "\n\n";
  oss << "必ず stdout の最後に以下のいずれか 1 つの JSON を出力してください。\n";
  oss << "{\"verdict\": \"approve\", \"summary\": \"承認理由の要約\"}\n";
  oss << "{\"verdict\": \"rework\", \"issues\": [\"問題点1\"], \"suggestions\": [\"改善案1\"]}\n";
  oss << "{\"verdict\": \"blocked\", \"reason\": \"ブロック理由\", "
         "\"category\": \"spec_conflict|human_required|external_dependency|other\"}\n";
  return oss.str();
}

std::string read_text_file(const fs::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to read file: " + path.string());
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

void write_text_file(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
  out << content;
}

std::vector<std::string> extract_json_objects(const std::string& text) {
  std::vector<std::string> objects;
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  std::size_t start = std::string::npos;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }

    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == '{') {
      if (depth == 0) {
        start = i;
      }
      ++depth;
      continue;
    }
    if (ch == '}') {
      if (depth == 0) {
        continue;
      }
      --depth;
      if (depth == 0 && start != std::string::npos) {
        objects.push_back(text.substr(start, i - start + 1));
        start = std::string::npos;
      }
    }
  }
  return objects;
}

std::optional<ReviewerVerdict> parse_reviewer_verdict(const std::string& normalized_output) {
  const std::vector<std::string> objects = extract_json_objects(normalized_output);
  for (auto it = objects.rbegin(); it != objects.rend(); ++it) {
    if (it->find("\"verdict\"") == std::string::npos) {
      continue;
    }
    ReviewerVerdict verdict;
    verdict.verdict = json_read_required_string(*it, "verdict");
    if (verdict.verdict != "approve" && verdict.verdict != "rework" &&
        verdict.verdict != "blocked") {
      throw std::runtime_error("invalid reviewer verdict");
    }
    verdict.summary = json_read_optional_string(*it, "summary");
    verdict.issues = json_read_optional_string_array(*it, "issues").value_or(
        std::vector<std::string>{});
    verdict.suggestions = json_read_optional_string_array(*it, "suggestions").value_or(
        std::vector<std::string>{});
    verdict.reason = json_read_optional_string(*it, "reason");
    verdict.category = json_read_optional_string(*it, "category");
    return verdict;
  }
  return std::nullopt;
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

std::string build_meta_json(const RunMetaData& meta) {
  std::ostringstream oss;
  oss << "{\n";
  oss << "  \"id\": " << json_string(meta.run_id) << ",\n";
  oss << "  \"run_id\": " << json_string(meta.run_id) << ",\n";
  oss << "  \"task_id\": " << json_string(meta.task_id) << ",\n";
  oss << "  \"attempt\": " << meta.attempt_number << ",\n";
  oss << "  \"attempt_number\": " << meta.attempt_number << ",\n";
  oss << "  \"project\": " << json_string(meta.project) << ",\n";
  oss << "  \"task_title\": " << json_string(meta.task_title) << ",\n";
  oss << "  \"task_source_file\": " << json_string(meta.task_source_file) << ",\n";
  oss << "  \"task_source_line\": " << meta.task_source_line << ",\n";
  oss << "  \"task_original_line\": " << json_string(meta.task_original_line) << ",\n";
  oss << "  \"path_name\": " << json_string(meta.path_name) << ",\n";
  oss << "  \"working_directory\": " << json_string(meta.working_directory) << ",\n";
  oss << "  \"agent\": " << json_string(meta.agent) << ",\n";
  oss << "  \"role\": " << json_string(meta.role) << ",\n";
  oss << "  \"status\": " << json_string(meta.status) << ",\n";
  oss << "  \"started_at\": " << json_string(meta.started_at) << ",\n";
  oss << "  \"ended_at\": " << json_nullable_string(meta.ended_at) << ",\n";
  oss << "  \"exit_code\": ";
  if (meta.exit_code.has_value()) {
    oss << *meta.exit_code;
  } else {
    oss << "null";
  }
  oss << ",\n";
  oss << "  \"exit_reason\": " << json_nullable_string(meta.exit_reason);
  if (meta.coder_run_id.has_value()) {
    oss << ",\n  \"coder_run_id\": " << json_string(*meta.coder_run_id);
  }
  if (meta.review_cycle.has_value()) {
    oss << ",\n  \"review_cycle\": " << *meta.review_cycle;
  }
  if (meta.verdict.has_value()) {
    oss << ",\n  \"verdict\": " << json_string(*meta.verdict);
  }
  oss << "\n}\n";
  return oss.str();
}

RunMetaData build_run_meta_data(
    const std::string& run_id,
    const std::string& project_name,
    const TaskState& task,
    const SelectedProjectPath& selected_path,
    const std::string& logical_agent_name,
    const std::string& role,
    const int attempt_number,
    const std::string& status,
    const std::string& started_at,
    const std::optional<std::string>& ended_at,
    const std::optional<int>& exit_code,
    const std::optional<std::string>& exit_reason,
    const std::optional<std::string>& coder_run_id,
    const std::optional<int>& review_cycle,
    const std::optional<std::string>& verdict) {
  RunMetaData meta;
  meta.run_id = run_id;
  meta.task_id = task.id;
  meta.attempt_number = attempt_number;
  meta.project = project_name;
  meta.task_title = task.title;
  meta.task_source_file = task.source_file;
  meta.task_source_line = task.source_line;
  meta.task_original_line = task.source_text;
  meta.path_name = selected_path.name;
  meta.working_directory = selected_path.path.string();
  meta.agent = logical_agent_name;
  meta.role = role;
  meta.status = status;
  meta.started_at = started_at;
  meta.ended_at = ended_at;
  meta.exit_code = exit_code;
  meta.exit_reason = exit_reason;
  meta.coder_run_id = coder_run_id;
  meta.review_cycle = review_cycle;
  meta.verdict = verdict;
  return meta;
}

std::optional<RunMetaData> parse_run_meta_file(const fs::path& meta_file) {
  if (!fs::exists(meta_file)) {
    return std::nullopt;
  }

  const std::string json = read_text_file(meta_file);
  RunMetaData meta;
  meta.run_id = json_read_optional_string(json, "id")
                    .value_or(json_read_required_string(json, "run_id"));
  meta.task_id = json_read_required_string(json, "task_id");
  meta.attempt_number =
      static_cast<int>(json_read_optional_integer(json, "attempt").value_or(
          json_read_optional_integer(json, "attempt_number").value_or(0)));
  meta.project = json_read_required_string(json, "project");
  meta.task_title = json_read_required_string(json, "task_title");
  meta.task_source_file = json_read_required_string(json, "task_source_file");
  meta.task_source_line =
      static_cast<std::size_t>(json_read_required_integer(json, "task_source_line"));
  meta.task_original_line = json_read_required_string(json, "task_original_line");
  meta.path_name = json_read_required_string(json, "path_name");
  meta.working_directory = json_read_required_string(json, "working_directory");
  meta.agent = json_read_required_string(json, "agent");
  meta.role = json_read_optional_string(json, "role").value_or("coder");
  meta.status = json_read_required_string(json, "status");
  meta.started_at = json_read_required_string(json, "started_at");
  meta.ended_at = json_read_optional_string(json, "ended_at");
  if (!nullable_integer_field_is_null(json, "exit_code")) {
    if (const std::optional<long long> exit_code = json_read_optional_integer(json, "exit_code");
        exit_code.has_value()) {
      meta.exit_code = static_cast<int>(*exit_code);
    }
  }
  meta.exit_reason = json_read_optional_string(json, "exit_reason");
  meta.coder_run_id = json_read_optional_string(json, "coder_run_id");
  if (const std::optional<long long> review_cycle = json_read_optional_integer(json, "review_cycle");
      review_cycle.has_value()) {
    meta.review_cycle = static_cast<int>(*review_cycle);
  }
  meta.verdict = json_read_optional_string(json, "verdict");
  return meta;
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
