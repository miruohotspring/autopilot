#include "autopilot/commands/cmd_start.hpp"

#include "autopilot/agents/agent_launcher.hpp"
#include "autopilot/commands/delete_ui.hpp"
#include "autopilot/projects/project_paths.hpp"
#include "autopilot/projects/project_store.hpp"
#include "autopilot/projects/todo_task_selector.hpp"
#include "autopilot/projects/todo_task_sync.hpp"
#include "autopilot/runtime/alert_store.hpp"
#include "autopilot/runtime/event_log.hpp"
#include "autopilot/runtime/json_utils.hpp"
#include "autopilot/runtime/lock_manager.hpp"
#include "autopilot/runtime/run_result_classifier.hpp"
#include "autopilot/runtime/task_state_store.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
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

namespace {

struct SelectedProjectPath {
  std::string name;
  fs::path path;
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

std::optional<RunMetaData> parse_run_meta_file(const fs::path& meta_file);
TaskState* find_task_by_id(std::vector<TaskState>& tasks, const std::string& task_id);

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
  std::tm local_tm{};
#if defined(_WIN32)
  localtime_s(&local_tm, &now_time);
#else
  localtime_r(&now_time, &local_tm);
#endif

  std::ostringstream oss;
  oss << "run-" << std::put_time(&local_tm, "%Y%m%d-%H%M%S") << "-L"
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
  oss << "You are working on autopilot project '" << project_name << "'.\n";
  oss << "Working directory: " << selected_path.path.string() << "\n";
  oss << "Managed path name: " << selected_path.name << "\n";
  oss << "Selected task: " << task.title << "\n";
  oss << "Source line in TODO.md: " << task.source_line << "\n";
  oss << "Original TODO line: " << task.source_text << "\n";
  oss << "Implement the selected task if possible.\n";
  oss << "You may inspect and edit files in the working directory.\n";
  oss << "At the end, print a short summary of what you changed.\n";
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

std::string sanitize_tmux_name(std::string s) {
  for (char& ch : s) {
    const bool is_alnum =
        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    if (!is_alnum && ch != '-' && ch != '_') {
      ch = '-';
    }
  }
  return s;
}

bool tmux_is_available() {
  return std::getenv("AUTOPILOT_START_DISABLE_TMUX") == nullptr &&
         std::system("tmux -V >/dev/null 2>&1") == 0;
}

void touch_empty_file(const fs::path& path) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to create file: " + path.string());
  }
}

int read_exit_code_file(const fs::path& exit_code_file) {
  std::ifstream in(exit_code_file);
  if (!in) {
    throw std::runtime_error("failed to read exit code file: " + exit_code_file.string());
  }
  int exit_code = 1;
  in >> exit_code;
  return exit_code;
}

int decode_system_exit_code(const int system_status) {
  if (system_status == -1) {
    return 1;
  }
  if (WIFEXITED(system_status)) {
    return WEXITSTATUS(system_status);
  }
  if (WIFSIGNALED(system_status)) {
    return 128 + WTERMSIG(system_status);
  }
  return 1;
}

pid_t launch_bash_command_async(const std::string& command) {
  const pid_t child_pid = ::fork();
  if (child_pid < 0) {
    throw std::runtime_error("failed to fork runner process");
  }
  if (child_pid == 0) {
    ::execl("/bin/bash", "bash", "-lc", command.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  return child_pid;
}

int wait_for_process(const pid_t pid) {
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    throw std::runtime_error("failed to wait for runner process");
  }
  return decode_system_exit_code(status);
}

std::optional<int> wait_for_pid_file(const fs::path& pid_file, int attempts = 500) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (fs::exists(pid_file)) {
      std::ifstream in(pid_file);
      int pid = 0;
      if (in >> pid && pid > 0) {
        return pid;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return std::nullopt;
}

std::string build_logged_agent_command(
    const std::string& agent_name,
    const std::string& prompt,
    const fs::path& working_directory,
    const std::string& stdout_redirection,
    const std::string& stderr_redirection) {
  return "cd " + shell_quote(working_directory.string()) + " && exec " +
         build_agent_shell_command(agent_name, prompt) + stdout_redirection + stderr_redirection;
}

std::string build_timed_shell_fragment(
    const std::string& command,
    const fs::path& timeout_marker_file,
    int timeout_seconds) {
  if (timeout_seconds <= 0) {
    return command + "; status=$?";
  }

  const std::string marker = shell_quote(timeout_marker_file.string());
  const std::string wrapped_command = shell_quote("set +e; " + command);
  std::ostringstream oss;
  oss << "rm -f " << marker << "; ";
  oss << "setsid bash -lc " << wrapped_command << " & cmd_pid=$!; ";
  oss << "(sleep " << timeout_seconds
      << "; if kill -0 \"$cmd_pid\" 2>/dev/null; then : > " << marker
      << "; kill -TERM -- -\"$cmd_pid\" 2>/dev/null; sleep 1; "
      << "kill -KILL -- -\"$cmd_pid\" 2>/dev/null || true; "
      << "fi) & watchdog_pid=$!; ";
  oss << "wait \"$cmd_pid\"; status=$?; ";
  oss << "kill \"$watchdog_pid\" 2>/dev/null || true; ";
  oss << "wait \"$watchdog_pid\" 2>/dev/null || true";
  return oss.str();
}

AgentLaunchResult run_agent_in_tmux(
    const std::string& project_name,
    const std::string& run_id,
    const std::string& agent_name,
    const std::string& prompt,
    const fs::path& working_directory,
    const fs::path& stdout_log,
    const fs::path& stderr_log,
    int timeout_seconds,
    const std::function<void(int)>& on_runner_started) {
  const std::string session = "autopilot";
  const std::string window_name = sanitize_tmux_name("start-" + project_name + "-" + run_id);
  const std::string target = session + ":" + window_name;
  const std::string channel = sanitize_tmux_name("ap-start-" + project_name + "-" + run_id);
  const fs::path exit_code_file = stdout_log.parent_path() / "agent.exit";
  const fs::path runner_pid_file = stdout_log.parent_path() / "runner.pid";

  touch_empty_file(stdout_log);
  touch_empty_file(stderr_log);
  touch_empty_file(exit_code_file);
  fs::remove(runner_pid_file);

  const bool session_exists =
      std::system(("tmux has-session -t " + session + " >/dev/null 2>&1").c_str()) == 0;

  // For claude (stream-json output), write a display filter that converts JSON events to
  // human-readable text for the tmux window, while the raw JSON is still captured to stdout.log.
  std::string stdout_display;
  if (agent_name == "claude") {
    const fs::path filter_script = stdout_log.parent_path() / "display_filter.py";
    write_text_file(filter_script, "import sys\nimport json\n\nfor line in sys.stdin:\n"
                                   "    line = line.strip()\n"
                                   "    if not line:\n"
                                   "        continue\n"
                                   "    try:\n"
                                   "        event = json.loads(line)\n"
                                   "    except Exception:\n"
                                   "        continue\n"
                                   "    if event.get(\"type\") != \"assistant\":\n"
                                   "        continue\n"
                                   "    for block in event.get(\"message\", {}).get(\"content\", []):\n"
                                   "        block_type = block.get(\"type\")\n"
                                   "        if block_type == \"thinking\":\n"
                                   "            text = block.get(\"thinking\", \"\").strip()\n"
                                   "            if text:\n"
                                   "                print(\"[thinking] \" + text, flush=True)\n"
                                   "        elif block_type == \"text\":\n"
                                   "            text = block.get(\"text\", \"\").strip()\n"
                                   "            if text:\n"
                                   "                print(text, flush=True)\n"
                                   "        elif block_type == \"tool_use\":\n"
                                   "            name = block.get(\"name\", \"?\")\n"
                                   "            inp = block.get(\"input\", {})\n"
                                   "            value = next(iter(inp.values()), \"\") if inp else \"\"\n"
                                   "            print(\"[\" + name + \"] \" + str(value)[:60], flush=True)\n");
    stdout_display = " | python3 " + shell_quote(filter_script.string());
  }

  const std::string agent_command = build_logged_agent_command(
      agent_name,
      prompt,
      working_directory,
      " > >(tee " + shell_quote(stdout_log.string()) + stdout_display + ")",
      " 2> >(tee " + shell_quote(stderr_log.string()) + " >&2)");
  const std::string timed_agent_command = build_timed_shell_fragment(
      agent_command,
      stdout_log.parent_path() / "agent.timeout",
      timeout_seconds);
  const std::string worker_script =
      "set +e; printf '%s\\n' \"$$\" > " + shell_quote(runner_pid_file.string()) + "; " +
      timed_agent_command + "; printf '%s\\n' \"$status\" > " +
      shell_quote(exit_code_file.string()) + "; tmux wait-for -S " + shell_quote(channel) +
      "; tmux kill-window -t " + shell_quote(target);
  const std::string tmux_command = "bash -lc " + shell_quote(worker_script);

  if (!session_exists) {
    const std::string create_session_cmd =
        "tmux new-session -d -s " + session + " -n " + shell_quote(window_name) + " " +
        shell_quote(tmux_command);
    if (std::system(create_session_cmd.c_str()) != 0) {
      throw std::runtime_error("failed to create tmux session");
    }
  } else {
    const std::string create_window_cmd =
        "tmux new-window -d -t " + session + " -n " + shell_quote(window_name) + " " +
        shell_quote(tmux_command);
    if (std::system(create_window_cmd.c_str()) != 0) {
      throw std::runtime_error("failed to create tmux window");
    }
  }

  const std::optional<int> runner_pid = wait_for_pid_file(runner_pid_file);
  if (!runner_pid.has_value()) {
    throw std::runtime_error("failed to read tmux runner pid");
  }
  on_runner_started(*runner_pid);
  fs::remove(runner_pid_file);

  if (std::getenv("TMUX") != nullptr) {
    const std::string switch_cmd = "tmux switch-client -t " + shell_quote(target);
    if (std::system(switch_cmd.c_str()) != 0) {
      throw std::runtime_error("failed to switch tmux client");
    }
    if (std::system(("tmux wait-for " + shell_quote(channel)).c_str()) != 0) {
      throw std::runtime_error("failed to wait for tmux task completion");
    }
  } else if (!session_exists) {
    const std::string attach_cmd = "tmux attach-session -t " + shell_quote(target);
    if (std::system(attach_cmd.c_str()) != 0) {
      throw std::runtime_error("failed to attach tmux session");
    }
  } else {
    std::cout << "started task window: " << target << '\n';
    if (std::system(("tmux wait-for " + shell_quote(channel)).c_str()) != 0) {
      throw std::runtime_error("failed to wait for tmux task completion");
    }
  }

  const int exit_code = read_exit_code_file(exit_code_file);
  fs::remove(exit_code_file);
  return AgentLaunchResult{agent_name, exit_code};
}

AgentLaunchResult run_agent_with_timeout(
    const std::string& agent_name,
    const std::string& prompt,
    const fs::path& working_directory,
    const fs::path& stdout_log,
    const fs::path& stderr_log,
    const fs::path& timeout_marker_file,
    int timeout_seconds) {
  const std::string base_cmd = build_logged_agent_command(
      agent_name,
      prompt,
      working_directory,
      " >" + shell_quote(stdout_log.string()),
      " 2>" + shell_quote(stderr_log.string()));
  const std::string wrapped_command =
      "set +e; " +
      build_timed_shell_fragment(base_cmd, timeout_marker_file, timeout_seconds) +
      "; exit \"$status\"";
  const pid_t runner_pid = launch_bash_command_async(wrapped_command);
  return AgentLaunchResult{agent_name, wait_for_process(runner_pid)};
}

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
    const std::optional<std::string>& coder_run_id = std::nullopt,
    const std::optional<int>& review_cycle = std::nullopt,
    const std::optional<std::string>& verdict = std::nullopt) {
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
    if (task.reviewer_run_id.has_value()) {
      if (const std::optional<RunMetaData> meta =
              parse_run_meta_file(runs_dir / *task.reviewer_run_id / "meta.json");
          meta.has_value() && meta->role == "reviewer") {
        finalize_interrupted_reviewer_run(runs_dir, *meta, recovered_at);
        task.latest_run_id = meta->run_id;
        task.reviewer_run_id = meta->run_id;
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
    const TaskUpdateChange& change) {
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
          "ap.start",
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
    const std::string& reason) {
  event_log.append(
      project_name,
      EventRecord{
          task.id,
          run_id,
          "task.status_changed",
          "ap.start",
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

} // namespace

int cmd_start(
    const std::optional<std::string>& maybe_project_name,
    std::optional<int> maybe_timeout_seconds,
    std::optional<bool> maybe_review_enabled,
    std::optional<std::string> maybe_reviewer_agent) {
  try {
    const std::string project_name = resolve_project_name(maybe_project_name);
    const fs::path project_dir = project_dir_path(project_name);
    const fs::path projects_file = projects_file_path();
    const ProjectConfig project_config = load_required_project_config(projects_file, project_name);
    const SelectedProjectPath selected_path = resolve_project_path(projects_file, project_name);
    const fs::path todo_file = project_dir / "TODO.md";
    if (!fs::exists(todo_file)) {
      std::cerr << "ap start failed: TODO.md not found\n";
      return 1;
    }

    const fs::path runtime_dir = project_dir / "runtime";
    const fs::path runs_dir = runtime_dir / "runs";
    const fs::path events_file = runtime_dir / "events" / "events.jsonl";
    const fs::path state_dir = runtime_dir / "state";
    const fs::path project_state_file = state_dir / "project.json";
    const fs::path tasks_dir = state_dir / "tasks";
    const fs::path lock_dir = runtime_dir / "lock";
    const fs::path alerts_dir = runtime_dir / "alerts";
    const fs::path last_run_file = runtime_dir / "last_run.json";
    fs::create_directories(runs_dir);
    fs::create_directories(tasks_dir);
    fs::create_directories(events_file.parent_path());
    fs::create_directories(lock_dir);

    EventLog event_log(events_file);
    const std::optional<ProjectState> previous_project_state = load_project_state(project_state_file);

    // Determine run_counter and default_timeout_seconds from previous project state
    int run_counter = previous_project_state.has_value() ? previous_project_state->run_counter : 0;
    const int default_timeout_seconds =
        previous_project_state.has_value() ? previous_project_state->default_timeout_seconds : 1800;
    const int timeout_seconds = maybe_timeout_seconds.value_or(default_timeout_seconds);

    const std::vector<ProjectPathEntry> project_paths =
        load_project_path_entries(projects_file, project_name);
    std::set<std::string> valid_path_names;
    for (const ProjectPathEntry& path_entry : project_paths) {
      valid_path_names.insert(path_entry.name);
    }

    const std::vector<TaskState> existing_tasks = load_task_states(tasks_dir);
    const std::string synced_at = current_timestamp_with_offset();

    TodoSyncResult sync_result;
    try {
      sync_result = sync_todo_with_task_state(
          todo_file, existing_tasks, synced_at, selected_path.name, project_config.slug);
    } catch (const std::runtime_error& e) {
      event_log.append(
          project_name,
          EventRecord{
              std::nullopt,
              std::nullopt,
              "todo.sync_conflict",
              "ap.start",
              {EventPayloadField{"message", json_string(e.what())}},
          });
      std::cerr << "ap start failed: " << e.what() << '\n';
      return 1;
    }

    validate_task_state(sync_result.tasks, valid_path_names);
    ProjectState project_state;
    int retry_exhausted_count = 0;
    const bool has_existing_in_progress = std::any_of(
        existing_tasks.begin(),
        existing_tasks.end(),
        [](const TaskState& task) { return task.status == "in_progress"; });
    const bool has_existing_review_pending = std::any_of(
        existing_tasks.begin(),
        existing_tasks.end(),
        [](const TaskState& task) { return task.status == "review_pending"; });
    bool has_orphaned_reviewer_run = false;
    for (const auto& entry : fs::directory_iterator(runs_dir)) {
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

    auto append_retry_exhausted_events = [&]() {
      for (const TaskState& task : sync_result.tasks) {
        if (!task.present_in_todo || !is_retry_exhausted(task)) {
          continue;
        }
        std::cerr << "ap start: task " << task.id << " has reached max retries ("
                  << task.attempt_count << "/" << task.max_retries << "), skipping\n";
        event_log.append(
            project_name,
            EventRecord{
                task.id,
                std::nullopt,
                "task.retry_exhausted",
                "ap.start",
                {
                    EventPayloadField{"attempt_count", std::to_string(task.attempt_count)},
                    EventPayloadField{"max_retries", std::to_string(task.max_retries)},
                },
            });
      }
    };

    auto print_no_runnable_message = [&]() {
      if (retry_exhausted_count > 0) {
        std::cerr << "ap start failed: no runnable task (" << retry_exhausted_count
                  << " task" << (retry_exhausted_count == 1 ? "" : "s")
                  << " exhausted max retries)\n";
      } else {
        std::cerr << "ap start failed: no runnable task\n";
      }
    };

    auto persist_sync_state = [&](const std::vector<TaskStatusChange>& recovered_in_progress_changes) {
      save_task_states(tasks_dir, sync_result.tasks);
      for (const TaskState& task : sync_result.discovered_tasks) {
        event_log.append(
            project_name,
            EventRecord{
                task.id,
                std::nullopt,
                "task.discovered",
                "ap.start",
                {
                    EventPayloadField{"title", json_string(task.title)},
                    EventPayloadField{"source_line", std::to_string(task.source_line)},
                },
            });
      }
      for (const TaskUpdateChange& change : sync_result.task_updates) {
        append_task_updated_event(event_log, project_name, change);
      }
      for (const TaskStatusChange& change : sync_result.status_changes) {
        TaskState* changed_task = find_task_by_id(sync_result.tasks, change.task_id);
        if (changed_task == nullptr) {
          continue;
        }
        append_status_changed_event(
            event_log,
            project_name,
            *changed_task,
            std::nullopt,
            change.from_status,
            change.to_status,
            "todo_sync");
      }
      for (const TaskStatusChange& change : recovered_in_progress_changes) {
        TaskState* changed_task = find_task_by_id(sync_result.tasks, change.task_id);
        if (changed_task == nullptr) {
          continue;
        }
        append_status_changed_event(
            event_log,
            project_name,
            *changed_task,
            std::nullopt,
            change.from_status,
            change.to_status,
            "stale_run_recovered");
      }
      for (const TaskStatusChange& change : recovered_in_progress_changes) {
        TaskState* changed_task = find_task_by_id(sync_result.tasks, change.task_id);
        if (changed_task == nullptr) {
          continue;
        }
        const std::optional<std::string> interrupted_run_id = changed_task->latest_run_id;
        event_log.append(
            project_name,
            EventRecord{
                changed_task->id,
                interrupted_run_id,
                "run.interrupted",
                "ap.start",
                {
                    EventPayloadField{
                        "run_id",
                        json_nullable_string(interrupted_run_id),
                    },
                    EventPayloadField{"detected_at", json_string(synced_at)},
                },
            });
      }

      project_state = build_project_state(
          project_name,
          sync_result.tasks,
          previous_project_state,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          run_counter,
          default_timeout_seconds,
          synced_at);
      save_project_state(project_state_file, project_state);
    };

    std::vector<TaskStatusChange> recovered_in_progress_changes;
    bool sync_state_persisted = false;
    if (has_existing_in_progress || has_existing_review_pending || has_orphaned_reviewer_run) {
      LockManager sync_lock_manager(lock_dir);
      bool sync_lock_was_stale = false;
      LockInfo sync_stale_info{};
      const bool sync_lock_acquired = sync_lock_manager.acquire_project_lock(
          allocate_run_id_with_counter(runs_dir, 0),
          timeout_seconds,
          sync_lock_was_stale,
          sync_stale_info);

      if (sync_lock_was_stale) {
        event_log.append(
            project_name,
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
        recovered_in_progress_changes = recover_stale_tasks(sync_result.tasks, runs_dir, synced_at);
        append_retry_exhausted_events();
        persist_sync_state(recovered_in_progress_changes);
        sync_state_persisted = true;
      } catch (const std::exception&) {
        sync_lock_manager.release_project_lock();
        throw;
      }
      sync_lock_manager.release_project_lock();
    }

    TaskState* selected_task =
        select_runnable_task(sync_result.tasks, selected_path.name, retry_exhausted_count);

    if (selected_task == nullptr) {
      if (!sync_state_persisted) {
        append_retry_exhausted_events();
        persist_sync_state(recovered_in_progress_changes);
      }
      print_no_runnable_message();
      return 1;
    }

    const std::string agent_name = resolve_agent_name();
    const std::string reviewer_agent_name = resolve_reviewer_agent_name(maybe_reviewer_agent, agent_name);
    const std::string coder_agent = coder_logical_agent_name(agent_name);
    const std::string reviewer_agent = reviewer_logical_agent_name(reviewer_agent_name);

    // Generate run_id using the incremented run_counter
    ++run_counter;
    const std::string run_id = allocate_run_id_with_counter(runs_dir, run_counter);
    const fs::path run_dir = runs_dir / run_id;
    fs::create_directories(run_dir);

    const fs::path meta_file = run_dir / "meta.json";
    const fs::path prompt_file = run_dir / "prompt.txt";
    const fs::path stdout_file = run_dir / "stdout.log";
    const fs::path stderr_file = run_dir / "stderr.log";
    const fs::path result_file = run_dir / "result.json";
    const fs::path timeout_marker_file = run_dir / "agent.timeout";

    // --- Phase 5: Lock acquisition ---
    LockManager lock_manager(lock_dir);

    // Acquire project lock
    bool proj_lock_was_stale = false;
    LockInfo proj_stale_info{};
    const bool proj_lock_acquired =
        lock_manager.acquire_project_lock(run_id, timeout_seconds, proj_lock_was_stale, proj_stale_info);

    if (proj_lock_was_stale) {
      event_log.append(
          project_name,
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
        project_name,
        EventRecord{
            std::nullopt,
            run_id,
            "lock.acquired",
            "ap.start",
            {
                EventPayloadField{"lock_type", json_string("project")},
                EventPayloadField{"run_id", json_string(run_id)},
            },
        });

    if (!sync_state_persisted) {
      append_retry_exhausted_events();
      persist_sync_state(recovered_in_progress_changes);
    }

    // Acquire task lock
    const bool task_lock_acquired =
        lock_manager.acquire_task_lock(selected_task->id, run_id, timeout_seconds);
    if (!task_lock_acquired) {
      lock_manager.release_project_lock();
      std::cerr << "ap start failed: task " << selected_task->id << " is already locked\n";
      return 1;
    }

    event_log.append(
        project_name,
        EventRecord{
            selected_task->id,
            run_id,
            "lock.acquired",
            "ap.start",
            {
                EventPayloadField{"lock_type", json_string("task")},
                EventPayloadField{"task_id", json_string(selected_task->id)},
                EventPayloadField{"run_id", json_string(run_id)},
            },
        });

    // --- Update task state and begin run ---
    const std::string started_at = current_timestamp_with_offset();
    const TaskState previous_task_state = *selected_task;
    const std::string previous_status = selected_task->status;
    selected_task->status = "in_progress";
    ++selected_task->attempt_count;
    selected_task->latest_run_id = run_id;
    selected_task->last_run_exit_reason = std::nullopt;
    selected_task->last_error = std::nullopt;
    selected_task->blocker_reason = std::nullopt;
    selected_task->blocker_category = std::nullopt;
    selected_task->updated_at = started_at;
    save_task_state(tasks_dir, *selected_task);

    project_state = build_project_state(
        project_name,
        sync_result.tasks,
        previous_project_state,
        selected_task->id,
        run_id,
        std::nullopt,
        std::nullopt,
        run_counter,
        default_timeout_seconds,
        started_at);
    AgentLaunchResult launch_result{agent_name, 1};
    bool launch_started = false;
    const bool review_enabled_for_task =
        review_flow_enabled_for_task(*selected_task, project_state, maybe_review_enabled);
    const std::string prompt = build_prompt(project_name, selected_path, *selected_task);
    std::chrono::steady_clock::time_point started_clock;
    try {
      save_project_state(project_state_file, project_state);

      event_log.append(
          project_name,
          EventRecord{
              selected_task->id,
              run_id,
              "task.selected",
              "ap.start",
              {
                  EventPayloadField{"reason", json_string("first_runnable_task")},
                  EventPayloadField{"previous_status", json_string(previous_status)},
                  EventPayloadField{"source_line", std::to_string(selected_task->source_line)},
                  EventPayloadField{
                      "attempt_number",
                      std::to_string(selected_task->attempt_count),
                  },
              },
          });
      append_status_changed_event(
          event_log,
          project_name,
          *selected_task,
          run_id,
          previous_status,
          selected_task->status,
          "run_started");

      write_text_file(prompt_file, prompt);
      write_text_file(
          meta_file,
          build_meta_json(build_run_meta_data(
              run_id,
              project_name,
              *selected_task,
              selected_path,
              coder_agent,
              "coder",
              selected_task->attempt_count,
              "running",
              started_at,
              std::nullopt,
              std::nullopt,
              std::nullopt)));
      event_log.append(
          project_name,
          EventRecord{
              selected_task->id,
              run_id,
              "run.started",
              "ap.start",
              {
                  EventPayloadField{"agent", json_string(coder_agent)},
                  EventPayloadField{"path_name", json_string(selected_path.name)},
                  EventPayloadField{
                      "working_directory",
                      json_string(selected_path.path.string()),
                  },
                  EventPayloadField{"attempt", std::to_string(selected_task->attempt_count)},
                  EventPayloadField{"role", json_string("coder")},
              },
          });

      started_clock = std::chrono::steady_clock::now();

      // For direct (non-tmux) run, wrap command with timeout
      if (tmux_is_available()) {
        launch_result = run_agent_in_tmux(
            project_name,
            run_id,
            agent_name,
            prompt,
            selected_path.path,
            stdout_file,
            stderr_file,
            timeout_seconds,
            [&](const int runner_pid) {
              lock_manager.transfer_project_lock_pid(runner_pid);
              lock_manager.transfer_task_lock_pid(selected_task->id, runner_pid);
              launch_started = true;
            });
      } else {
        // Build command with optional timeout wrapper for direct execution
        const std::string base_cmd = build_logged_agent_command(
            agent_name,
            prompt,
            selected_path.path,
            " >" + shell_quote(stdout_file.string()),
            " 2>" + shell_quote(stderr_file.string()));
        const std::string wrapped_command =
            "set +e; " +
            build_timed_shell_fragment(base_cmd, timeout_marker_file, timeout_seconds) +
            "; exit \"$status\"";
        const pid_t runner_pid = launch_bash_command_async(wrapped_command);
        lock_manager.transfer_project_lock_pid(static_cast<int>(runner_pid));
        lock_manager.transfer_task_lock_pid(selected_task->id, static_cast<int>(runner_pid));
        launch_started = true;
        launch_result = AgentLaunchResult{agent_name, wait_for_process(runner_pid)};
      }
    } catch (const std::exception&) {
      if (!launch_started) {
        rollback_prelaunch_task_update(
            tasks_dir,
            project_state_file,
            project_name,
            sync_result.tasks,
            *selected_task,
            previous_task_state,
            previous_project_state,
            run_counter,
            default_timeout_seconds,
            current_timestamp_with_offset());
        lock_manager.release_task_lock(selected_task->id);
        lock_manager.release_project_lock();
      }
      throw;
    }

    lock_manager.transfer_project_lock_pid(static_cast<int>(::getpid()));
    lock_manager.transfer_task_lock_pid(selected_task->id, static_cast<int>(::getpid()));

    const auto ended_clock = std::chrono::steady_clock::now();
    const std::string ended_at = current_timestamp_with_offset();
    const long long duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     ended_clock - started_clock)
                                     .count();
    const std::string pre_final_status = selected_task->status;

    RunResultClassification classification;
    bool succeeded = false;
    bool blocked = false;
    bool timed_out = false;
    bool todo_update_applied = false;
    std::optional<AlertRecord> created_alert;
    std::string exit_reason;
    try {
      timed_out = fs::exists(timeout_marker_file);
      if (timed_out) {
        fs::remove(timeout_marker_file);
      }

      if (timed_out) {
        exit_reason = "timeout";
        // Record run.timeout event
        event_log.append(
            project_name,
            EventRecord{
                selected_task->id,
                run_id,
                "run.timeout",
                "ap.start",
                {
                    EventPayloadField{"task_id", json_string(selected_task->id)},
                    EventPayloadField{"run_id", json_string(run_id)},
                    EventPayloadField{"timeout_seconds", std::to_string(timeout_seconds)},
                    EventPayloadField{"pid", std::to_string(static_cast<int>(::getpid()))},
                },
            });
        std::cerr << "ap start: task " << selected_task->id << " timed out after "
                  << timeout_seconds << " seconds\n";

        // Timeout is treated as failure
        classification.final_task_status = "failed";
        classification.process_status = "timeout";
        classification.summary_excerpt = "timed out after " + std::to_string(timeout_seconds) + "s";
      } else {
        rewrite_stream_json_stdout_log(stdout_file, stderr_file);
        classification = classify_run_result(launch_result.exit_code, stdout_file, stderr_file);
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

      selected_task->status = should_run_review ? "review_pending" : classification.final_task_status;
      selected_task->last_run_exit_reason = exit_reason;
      selected_task->updated_at = ended_at;
      if (should_run_review || classification.final_task_status == "done") {
        selected_task->last_error = std::nullopt;
        selected_task->blocker_reason = std::nullopt;
        selected_task->blocker_category = std::nullopt;
      } else if (coder_blocked) {
        selected_task->last_error = classification.blocker_reason;
        selected_task->blocker_reason = classification.blocker_reason;
        selected_task->blocker_category = classification.blocker_category;
        if (classification.approval_required) {
          selected_task->approval_required = true;
        }
      } else if (timed_out) {
        selected_task->last_error = std::optional<std::string>("timed out after " +
                                                                std::to_string(timeout_seconds) + "s");
        selected_task->blocker_reason = std::nullopt;
        selected_task->blocker_category = std::nullopt;
      } else {
        selected_task->last_error =
          std::optional<std::string>("agent exited with status " +
                                       std::to_string(launch_result.exit_code));
        selected_task->blocker_reason = std::nullopt;
        selected_task->blocker_category = std::nullopt;
      }
      save_task_state(tasks_dir, *selected_task);

      project_state = build_project_state(
          project_name,
          sync_result.tasks,
          previous_project_state,
          std::nullopt,
          std::nullopt,
          run_id,
          ended_at,
          run_counter,
          default_timeout_seconds,
          ended_at);
      save_project_state(project_state_file, project_state);

      if (coder_succeeded && !should_run_review) {
        todo_update_applied = mark_todo_task_done(
            todo_file,
            TodoTaskSelection{
                selected_task->id,
                selected_task->title,
                selected_task->source_line,
                selected_task->source_text,
                false,
            });
        if (!todo_update_applied) {
          event_log.append(
              project_name,
              EventRecord{
                  selected_task->id,
                  run_id,
                  "todo.sync_conflict",
                  "ap.start",
                  {EventPayloadField{"message", json_string("failed to update TODO.md after run")}},
              });
        } else {
          selected_task->source_text = completed_todo_line(selected_task->source_text);
          save_task_state(tasks_dir, *selected_task);
        }
      }

      write_text_file(
          meta_file,
          build_meta_json(build_run_meta_data(
              run_id,
              project_name,
              *selected_task,
              selected_path,
              coder_agent,
              "coder",
              selected_task->attempt_count,
              "finished",
              started_at,
              ended_at,
              launch_result.exit_code,
              exit_reason)));

      event_log.append_stream_file(
          project_name,
          selected_task->id,
          run_id,
          "agent." + coder_agent,
          "stdout",
          stdout_file);
      event_log.append_stream_file(
          project_name,
          selected_task->id,
          run_id,
          "agent." + coder_agent,
          "stderr",
          stderr_file);

      event_log.append(
          project_name,
          EventRecord{
              selected_task->id,
              run_id,
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
          AlertStore alert_store(alerts_dir);
          created_alert = alert_store.create(
              project_name, selected_task->id, run_id, *classification.alert, ended_at);
        }

        event_log.append(
            project_name,
            EventRecord{
                selected_task->id,
                run_id,
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
              project_name,
              EventRecord{
                  selected_task->id,
                  run_id,
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
          run_id,
          *selected_task,
          selected_task->attempt_count,
          classification.process_status,
          launch_result.exit_code,
          started_at,
          ended_at,
          duration_ms,
          todo_update_applied,
          selected_task->status,
          classification.summary_excerpt,
          classification.blocker_reason,
          created_alert.has_value() ? std::optional<std::string>(created_alert->id) : std::nullopt);
      write_text_file(result_file, result_json);
      write_text_file(last_run_file, result_json);

      append_status_changed_event(
          event_log,
          project_name,
          *selected_task,
          run_id,
          pre_final_status,
          selected_task->status,
          "result_finalized");
      event_log.append(
          project_name,
          EventRecord{
              selected_task->id,
              run_id,
              "result.final",
              "runtime.classifier",
              {
                  EventPayloadField{
                      "final_task_status",
                      json_string(selected_task->status),
                  },
                  EventPayloadField{
                      "process_exit_code",
                      std::to_string(launch_result.exit_code),
                  },
                  EventPayloadField{
                      "process_status",
                      json_string(classification.process_status),
                  },
                  EventPayloadField{
                      "summary",
                      json_string(classification.summary_excerpt),
                  },
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
        ++run_counter;
        const std::string reviewer_run_id = allocate_run_id_with_counter(runs_dir, run_counter);
        const fs::path reviewer_run_dir = runs_dir / reviewer_run_id;
        fs::create_directories(reviewer_run_dir);
        const fs::path reviewer_meta_file = reviewer_run_dir / "meta.json";
        const fs::path reviewer_prompt_file = reviewer_run_dir / "prompt.txt";
        const fs::path reviewer_stdout_file = reviewer_run_dir / "stdout.log";
        const fs::path reviewer_stderr_file = reviewer_run_dir / "stderr.log";
        const fs::path reviewer_result_file = reviewer_run_dir / "result.json";
        const fs::path reviewer_timeout_marker_file = reviewer_run_dir / "agent.timeout";
        const std::string reviewer_started_at = current_timestamp_with_offset();
        const int review_cycle = selected_task->review_cycle_count + 1;
        const std::string reviewer_prompt =
            build_reviewer_prompt(*selected_task, run_id, exit_reason, read_text_file(stdout_file));

        write_text_file(reviewer_prompt_file, reviewer_prompt);
        write_text_file(
            reviewer_meta_file,
            build_meta_json(build_run_meta_data(
                reviewer_run_id,
                project_name,
                *selected_task,
                selected_path,
                reviewer_agent,
                "reviewer",
                selected_task->attempt_count,
                "starting",
                reviewer_started_at,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                run_id,
                review_cycle)));

        selected_task->latest_run_id = reviewer_run_id;
        selected_task->reviewer_run_id = reviewer_run_id;
        selected_task->updated_at = reviewer_started_at;
        save_task_state(tasks_dir, *selected_task);

        project_state = build_project_state(
            project_name,
            sync_result.tasks,
            previous_project_state,
            selected_task->id,
            reviewer_run_id,
            run_id,
            ended_at,
            run_counter,
            default_timeout_seconds,
            reviewer_started_at);
        save_project_state(project_state_file, project_state);

        AgentLaunchResult reviewer_launch_result{reviewer_agent_name, 1};
        std::chrono::steady_clock::time_point reviewer_started_clock;
        try {
          write_text_file(
              reviewer_meta_file,
              build_meta_json(build_run_meta_data(
                  reviewer_run_id,
                  project_name,
                  *selected_task,
                  selected_path,
                  reviewer_agent,
                  "reviewer",
                  selected_task->attempt_count,
                  "running",
                  reviewer_started_at,
                  std::nullopt,
                  std::nullopt,
                  std::nullopt,
                  run_id,
                  review_cycle)));
          event_log.append(
              project_name,
              EventRecord{
                  selected_task->id,
                  reviewer_run_id,
                  "review.started",
                  reviewer_agent,
                  {
                      EventPayloadField{"coder_run_id", json_string(run_id)},
                      EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                      EventPayloadField{"agent", json_string(reviewer_agent)},
                  },
              });
          reviewer_started_clock = std::chrono::steady_clock::now();
          reviewer_launch_result = run_agent_with_timeout(
              reviewer_agent_name,
              reviewer_prompt,
              selected_path.path,
              reviewer_stdout_file,
              reviewer_stderr_file,
              reviewer_timeout_marker_file,
              timeout_seconds);
        } catch (const std::exception& reviewer_launch_error) {
          const std::string reviewer_ended_at = current_timestamp_with_offset();
          const std::string reviewer_exit_reason = "spawn_failed";
          const std::string reason = "reviewer failed to start: " +
                                     std::string(reviewer_launch_error.what());
          selected_task->status = "blocked";
          selected_task->last_run_exit_reason = reviewer_exit_reason;
          selected_task->last_error = reason;
          selected_task->blocker_reason = reason;
          selected_task->blocker_category = "reviewer_error";
          selected_task->review_result = "reviewer_error";
          selected_task->updated_at = reviewer_ended_at;
          save_task_state(tasks_dir, *selected_task);

          project_state = build_project_state(
              project_name,
              sync_result.tasks,
              previous_project_state,
              std::nullopt,
              std::nullopt,
              reviewer_run_id,
              reviewer_ended_at,
              run_counter,
              default_timeout_seconds,
              reviewer_ended_at);
          save_project_state(project_state_file, project_state);

          created_alert = AlertStore(alerts_dir).create(
              project_name,
              selected_task->id,
              reviewer_run_id,
              AlertDraft{"medium", "reviewer_error", reason},
              reviewer_ended_at);

          write_text_file(
              reviewer_meta_file,
              build_meta_json(build_run_meta_data(
                  reviewer_run_id,
                  project_name,
                  *selected_task,
                  selected_path,
                  reviewer_agent,
                  "reviewer",
                  selected_task->attempt_count,
                  "finished",
                  reviewer_started_at,
                  reviewer_ended_at,
                  std::nullopt,
                  reviewer_exit_reason,
                  run_id,
                  review_cycle)));
          const std::string reviewer_result_json = build_result_json(
              reviewer_run_id,
              *selected_task,
              selected_task->attempt_count,
              "failed",
              1,
              reviewer_started_at,
              reviewer_ended_at,
              0,
              false,
              selected_task->status,
              reason,
              selected_task->blocker_reason,
              created_alert->id);
          write_text_file(reviewer_result_file, reviewer_result_json);
          write_text_file(last_run_file, reviewer_result_json);
          append_status_changed_event(
              event_log,
              project_name,
              *selected_task,
              reviewer_run_id,
              "review_pending",
              selected_task->status,
              "reviewer_error");
          event_log.append(
              project_name,
              EventRecord{
                  selected_task->id,
                  reviewer_run_id,
                  "review.blocked",
                  reviewer_agent,
                  {
                      EventPayloadField{"task_id", json_string(selected_task->id)},
                      EventPayloadField{"reviewer_run_id", json_string(reviewer_run_id)},
                      EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                      EventPayloadField{"reason", json_string(reason)},
                      EventPayloadField{"category", json_string("reviewer_error")},
                      EventPayloadField{"reviewer_error", "true"},
                  },
              });
          event_log.append(
              project_name,
              EventRecord{
                  selected_task->id,
                  reviewer_run_id,
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
          succeeded = false;
          timed_out = false;
          classification.blocker_reason = reason;
          classification.blocker_category = "reviewer_error";
          classification.summary_excerpt = reason;
        }

        if (!blocked) {
          const auto reviewer_ended_clock = std::chrono::steady_clock::now();
          const std::string reviewer_ended_at = current_timestamp_with_offset();
          const long long reviewer_duration_ms =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  reviewer_ended_clock - reviewer_started_clock)
                  .count();
          std::string reviewer_exit_reason;
          std::string reviewer_process_status = reviewer_launch_result.exit_code == 0 ? "succeeded" : "failed";
          std::optional<ReviewerVerdict> reviewer_verdict;

          if (fs::exists(reviewer_timeout_marker_file)) {
            fs::remove(reviewer_timeout_marker_file);
            reviewer_exit_reason = "timeout";
            reviewer_process_status = "timeout";
          } else {
            rewrite_stream_json_stdout_log(reviewer_stdout_file, reviewer_stderr_file);
            if (reviewer_launch_result.exit_code == 0) {
              try {
                reviewer_verdict = parse_reviewer_verdict(
                    strip_known_agent_prefix(trim_ascii_whitespace(read_text_file(reviewer_stdout_file))));
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
              selected_task->status = "done";
              selected_task->review_result = "approve";
              selected_task->last_error = std::nullopt;
              selected_task->blocker_reason = std::nullopt;
              selected_task->blocker_category = std::nullopt;
              selected_task->review_feedback = std::nullopt;
              todo_update_applied = mark_todo_task_done(
                  todo_file,
                  TodoTaskSelection{
                      selected_task->id,
                      selected_task->title,
                      selected_task->source_line,
                      selected_task->source_text,
                      false,
                  });
              if (todo_update_applied) {
                selected_task->source_text = completed_todo_line(selected_task->source_text);
              } else {
                event_log.append(
                    project_name,
                    EventRecord{
                        selected_task->id,
                        reviewer_run_id,
                        "todo.sync_conflict",
                        "ap.start",
                        {EventPayloadField{
                            "message",
                            json_string("failed to update TODO.md after review approval"),
                        }},
                    });
              }
              event_log.append(
                  project_name,
                  EventRecord{
                      selected_task->id,
                      reviewer_run_id,
                      "review.approved",
                      reviewer_agent,
                      {
                          EventPayloadField{"task_id", json_string(selected_task->id)},
                          EventPayloadField{"reviewer_run_id", json_string(reviewer_run_id)},
                          EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                          EventPayloadField{
                              "summary",
                              json_string(reviewer_verdict->summary.value_or("approved")),
                          },
                      },
                  });
              succeeded = true;
              blocked = false;
            } else if (reviewer_verdict->verdict == "rework") {
              ++selected_task->review_cycle_count;
              selected_task->review_result = "rework";
              selected_task->review_feedback = TaskState::ReviewFeedback{
                  selected_task->review_cycle_count,
                  reviewer_run_id,
                  reviewer_verdict->issues,
                  reviewer_verdict->suggestions,
                  reviewer_ended_at,
              };
              if (selected_task->review_cycle_count >
                  effective_max_review_cycles(*selected_task, project_state)) {
                const std::string reason =
                    "max review cycles exceeded (" +
                    std::to_string(selected_task->review_cycle_count) + "/" +
                    std::to_string(effective_max_review_cycles(*selected_task, project_state)) + ")";
                selected_task->status = "blocked";
                selected_task->last_error = reason;
                selected_task->blocker_reason = reason;
                selected_task->blocker_category = "review_cycle_limit";
                created_alert = AlertStore(alerts_dir).create(
                    project_name,
                    selected_task->id,
                    reviewer_run_id,
                    AlertDraft{"medium", "review_cycle_exceeded", reason},
                    reviewer_ended_at);
                event_log.append(
                    project_name,
                    EventRecord{
                        selected_task->id,
                        reviewer_run_id,
                        "task.review_cycle_exceeded",
                        reviewer_agent,
                        {
                            EventPayloadField{"task_id", json_string(selected_task->id)},
                            EventPayloadField{"reviewer_run_id", json_string(reviewer_run_id)},
                            EventPayloadField{
                                "review_cycle_count",
                                std::to_string(selected_task->review_cycle_count),
                            },
                            EventPayloadField{
                                "max_review_cycles",
                                std::to_string(
                                    effective_max_review_cycles(*selected_task, project_state)),
                            },
                        },
                    });
                classification.blocker_reason = reason;
                classification.blocker_category = "review_cycle_limit";
                blocked = true;
                succeeded = false;
              } else {
                selected_task->status = "todo";
                selected_task->last_run_exit_reason = std::nullopt;
                selected_task->last_error = std::nullopt;
                selected_task->blocker_reason = std::nullopt;
                selected_task->blocker_category = std::nullopt;
                event_log.append(
                    project_name,
                    EventRecord{
                        selected_task->id,
                        reviewer_run_id,
                        "review.rework_requested",
                        reviewer_agent,
                        {
                            EventPayloadField{"task_id", json_string(selected_task->id)},
                            EventPayloadField{"reviewer_run_id", json_string(reviewer_run_id)},
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
                succeeded = false;
                blocked = false;
              }
            } else {
              selected_task->status = "blocked";
              selected_task->review_result = "blocked";
              selected_task->last_error = reviewer_verdict->reason;
              selected_task->blocker_reason = reviewer_verdict->reason;
              selected_task->blocker_category =
                  reviewer_verdict->category.value_or("other");
              const std::string alert_message =
                  "reviewer blocked " + selected_task->id + ": " +
                  reviewer_verdict->reason.value_or("blocked");
              created_alert = AlertStore(alerts_dir).create(
                  project_name,
                  selected_task->id,
                  reviewer_run_id,
                  AlertDraft{"high", "reviewer_blocked", alert_message},
                  reviewer_ended_at);
              event_log.append(
                  project_name,
                  EventRecord{
                      selected_task->id,
                      reviewer_run_id,
                      "review.blocked",
                      reviewer_agent,
                      {
                          EventPayloadField{"task_id", json_string(selected_task->id)},
                          EventPayloadField{"reviewer_run_id", json_string(reviewer_run_id)},
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
              classification.blocker_reason = reviewer_verdict->reason;
              classification.blocker_category = reviewer_verdict->category;
              blocked = true;
              succeeded = false;
            }
          } else if (reviewer_exit_reason == "parse_error") {
            const std::string reason = "reviewer output could not be parsed";
            selected_task->status = "blocked";
            selected_task->review_result = "parse_error";
            selected_task->last_error = reason;
            selected_task->blocker_reason = reason;
            selected_task->blocker_category = "reviewer_error";
            created_alert = AlertStore(alerts_dir).create(
                project_name,
                selected_task->id,
                reviewer_run_id,
                AlertDraft{"medium", "reviewer_parse_error", reason},
                reviewer_ended_at);
            event_log.append(
                project_name,
                EventRecord{
                    selected_task->id,
                    reviewer_run_id,
                    "review.blocked",
                    reviewer_agent,
                    {
                        EventPayloadField{"task_id", json_string(selected_task->id)},
                        EventPayloadField{"reviewer_run_id", json_string(reviewer_run_id)},
                        EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                        EventPayloadField{"reason", json_string(reason)},
                        EventPayloadField{"category", json_string("reviewer_error")},
                        EventPayloadField{"parse_error", "true"},
                    },
                });
            classification.blocker_reason = reason;
            classification.blocker_category = "reviewer_error";
            blocked = true;
            succeeded = false;
          } else {
            const std::string reason =
                reviewer_exit_reason == "timeout"
                    ? "timed out after " + std::to_string(timeout_seconds) + " seconds"
                    : "reviewer exited with status " +
                          std::to_string(reviewer_launch_result.exit_code);
            selected_task->status = "blocked";
            selected_task->review_result = "reviewer_error";
            selected_task->last_error = reason;
            selected_task->blocker_reason = reason;
            selected_task->blocker_category = "reviewer_error";
            created_alert = AlertStore(alerts_dir).create(
                project_name,
                selected_task->id,
                reviewer_run_id,
                AlertDraft{"medium", "reviewer_error", reason},
                reviewer_ended_at);
            event_log.append(
                project_name,
                EventRecord{
                    selected_task->id,
                    reviewer_run_id,
                    "review.blocked",
                    reviewer_agent,
                    {
                        EventPayloadField{"task_id", json_string(selected_task->id)},
                        EventPayloadField{"reviewer_run_id", json_string(reviewer_run_id)},
                        EventPayloadField{"review_cycle", std::to_string(review_cycle)},
                        EventPayloadField{"reason", json_string(reason)},
                        EventPayloadField{"category", json_string("reviewer_error")},
                        EventPayloadField{"reviewer_error", "true"},
                    },
                });
            classification.blocker_reason = reason;
            classification.blocker_category = "reviewer_error";
            blocked = true;
            succeeded = false;
          }

          selected_task->last_run_exit_reason = reviewer_exit_reason;
          selected_task->updated_at = reviewer_ended_at;
          save_task_state(tasks_dir, *selected_task);

          project_state = build_project_state(
              project_name,
              sync_result.tasks,
              previous_project_state,
              std::nullopt,
              std::nullopt,
              reviewer_run_id,
              reviewer_ended_at,
              run_counter,
              default_timeout_seconds,
              reviewer_ended_at);
          save_project_state(project_state_file, project_state);

          write_text_file(
              reviewer_meta_file,
              build_meta_json(build_run_meta_data(
                  reviewer_run_id,
                  project_name,
                  *selected_task,
                  selected_path,
                  reviewer_agent,
                  "reviewer",
                  selected_task->attempt_count,
                  "finished",
                  reviewer_started_at,
                  reviewer_ended_at,
                  reviewer_launch_result.exit_code,
                  reviewer_exit_reason,
                  run_id,
                  review_cycle,
                  reviewer_verdict.has_value()
                      ? std::optional<std::string>(reviewer_verdict->verdict)
                      : std::nullopt)));

          event_log.append_stream_file(
              project_name,
              selected_task->id,
              reviewer_run_id,
              "agent." + reviewer_agent,
              "stdout",
              reviewer_stdout_file);
          event_log.append_stream_file(
              project_name,
              selected_task->id,
              reviewer_run_id,
              "agent." + reviewer_agent,
              "stderr",
              reviewer_stderr_file);
          event_log.append(
              project_name,
              EventRecord{
                  selected_task->id,
                  reviewer_run_id,
                  "run.finished",
                  "ap.start",
                  {
                      EventPayloadField{
                          "agent",
                          json_string(reviewer_agent),
                      },
                      EventPayloadField{"role", json_string("reviewer")},
                      EventPayloadField{
                          "exit_code",
                          std::to_string(reviewer_launch_result.exit_code),
                      },
                      EventPayloadField{
                          "duration_ms",
                          std::to_string(reviewer_duration_ms),
                      },
                  },
              });
          if (created_alert.has_value()) {
            event_log.append(
                project_name,
                EventRecord{
                    selected_task->id,
                    reviewer_run_id,
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
                  : classification.blocker_reason.value_or(reviewer_exit_reason);
          const std::string reviewer_result_json = build_result_json(
              reviewer_run_id,
              *selected_task,
              selected_task->attempt_count,
              reviewer_process_status,
              reviewer_launch_result.exit_code,
              reviewer_started_at,
              reviewer_ended_at,
              reviewer_duration_ms,
              todo_update_applied,
              selected_task->status,
              reviewer_summary,
              selected_task->blocker_reason,
              created_alert.has_value()
                  ? std::optional<std::string>(created_alert->id)
                  : std::nullopt);
          write_text_file(reviewer_result_file, reviewer_result_json);
          write_text_file(last_run_file, reviewer_result_json);
          append_status_changed_event(
              event_log,
              project_name,
              *selected_task,
              reviewer_run_id,
              "review_pending",
              selected_task->status,
              "review_finalized");
          event_log.append(
              project_name,
              EventRecord{
                  selected_task->id,
                  reviewer_run_id,
                  "result.final",
                  "runtime.classifier",
                  {
                      EventPayloadField{
                          "final_task_status",
                          json_string(selected_task->status),
                      },
                      EventPayloadField{
                          "process_exit_code",
                          std::to_string(reviewer_launch_result.exit_code),
                      },
                      EventPayloadField{
                          "process_status",
                          json_string(reviewer_process_status),
                      },
                      EventPayloadField{
                          "summary",
                          json_string(reviewer_summary),
                      },
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
      }
    } catch (const std::exception& e) {
      finalize_postrun_failure(
          tasks_dir,
          project_state_file,
          project_name,
          sync_result.tasks,
          *selected_task,
          previous_project_state,
          run_id,
          run_counter,
          default_timeout_seconds,
          "post-run processing failed: " + std::string(e.what()),
          current_timestamp_with_offset());
      // Release locks on exception
      event_log.append(
          project_name,
          EventRecord{
              selected_task->id,
              run_id,
              "lock.released",
              "ap.start",
              {EventPayloadField{"lock_type", json_string("task")}},
          });
      lock_manager.release_task_lock(selected_task->id);
      event_log.append(
          project_name,
          EventRecord{
              std::nullopt,
              run_id,
              "lock.released",
              "ap.start",
              {EventPayloadField{"lock_type", json_string("project")}},
          });
      lock_manager.release_project_lock();
      throw;
    }

    // Release task lock, then project lock
    event_log.append(
        project_name,
        EventRecord{
            selected_task->id,
            run_id,
            "lock.released",
            "ap.start",
            {EventPayloadField{"lock_type", json_string("task")}},
        });
    lock_manager.release_task_lock(selected_task->id);

    event_log.append(
        project_name,
        EventRecord{
            std::nullopt,
            run_id,
            "lock.released",
            "ap.start",
            {EventPayloadField{"lock_type", json_string("project")}},
        });
    lock_manager.release_project_lock();

    if (succeeded) {
      std::cout << "completed task: " << selected_task->title << '\n';
      return 0;
    }

    if (blocked) {
      std::cerr << "ap start blocked: "
                << classification.blocker_reason.value_or("blocked by external dependency") << '\n';
      return 1;
    }

    if (timed_out) {
      std::cerr << "ap start failed: task timed out after " << timeout_seconds << " seconds\n";
      return 1;
    }

    if (selected_task->status == "todo" && selected_task->review_result == "rework") {
      std::cerr << "ap start: reviewer returned rework for task " << selected_task->id
                << " (cycle " << selected_task->review_cycle_count << "/"
                << effective_max_review_cycles(*selected_task, project_state) << ")\n";
      return 1;
    }

    std::cerr << "ap start failed: agent exited with status " << launch_result.exit_code << '\n';
    return 1;
  } catch (const std::exception& e) {
    const std::string message = e.what();
    if (message == "Please run ap init first" || message == "project not found" ||
        message == "invalid project name") {
      std::cerr << message << '\n';
    } else if (message == "no managed path" || message == "project has multiple paths and no 'main'") {
      std::cerr << "ap start failed: " << message << '\n';
    } else {
      std::cerr << "ap start failed: " << message << '\n';
    }
    return 1;
  }
}
