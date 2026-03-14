#include "autopilot/commands/cmd_start.hpp"

#include "autopilot/agents/agent_launcher.hpp"
#include "autopilot/commands/delete_ui.hpp"
#include "autopilot/projects/project_paths.hpp"
#include "autopilot/projects/project_store.hpp"
#include "autopilot/projects/todo_task_selector.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <ctime>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct SelectedProjectPath {
  std::string name;
  fs::path path;
};

std::string json_escape(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (char ch : input) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::string json_string(const std::string& input) {
  return "\"" + json_escape(input) + "\"";
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

std::string current_run_id(const std::size_t source_line) {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_tm{};
#if defined(_WIN32)
  localtime_s(&local_tm, &now_time);
#else
  localtime_r(&now_time, &local_tm);
#endif

  std::ostringstream oss;
  oss << "run-" << std::put_time(&local_tm, "%Y%m%d-%H%M%S") << "-L" << source_line;
  return oss.str();
}

std::string build_prompt(
    const std::string& project_name,
    const SelectedProjectPath& selected_path,
    const TodoTaskSelection& task) {
  std::ostringstream oss;
  oss << "You are working on autopilot project '" << project_name << "'.\n";
  oss << "Working directory: " << selected_path.path.string() << "\n";
  oss << "Managed path name: " << selected_path.name << "\n";
  oss << "Selected task: " << task.title << "\n";
  oss << "Source line in TODO.md: " << task.source_line << "\n";
  oss << "Original TODO line: " << task.original_line_text << "\n";
  oss << "Implement the selected task if possible.\n";
  oss << "You may inspect and edit files in the working directory.\n";
  oss << "At the end, print a short summary of what you changed.\n";
  return oss.str();
}

std::string read_last_non_empty_line(const fs::path& file) {
  std::ifstream in(file);
  if (!in) {
    return "";
  }

  std::string line;
  std::string last_non_empty;
  while (std::getline(in, line)) {
    if (!line.empty()) {
      last_non_empty = line;
    }
  }
  return last_non_empty;
}

void write_text_file(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
  out << content;
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

std::string build_meta_json(
    const std::string& run_id,
    const std::string& project_name,
    const TodoTaskSelection& task,
    const SelectedProjectPath& selected_path,
    const std::string& agent_name,
    const std::string& status,
    const std::string& started_at,
    const std::optional<std::string>& ended_at,
    const std::optional<int>& exit_code) {
  std::ostringstream oss;
  oss << "{\n";
  oss << "  \"run_id\": " << json_string(run_id) << ",\n";
  oss << "  \"project\": " << json_string(project_name) << ",\n";
  oss << "  \"task_title\": " << json_string(task.title) << ",\n";
  oss << "  \"task_source_file\": \"TODO.md\",\n";
  oss << "  \"task_source_line\": " << task.source_line << ",\n";
  oss << "  \"task_original_line\": " << json_string(task.original_line_text) << ",\n";
  oss << "  \"path_name\": " << json_string(selected_path.name) << ",\n";
  oss << "  \"working_directory\": " << json_string(selected_path.path.string()) << ",\n";
  oss << "  \"agent\": " << json_string(agent_name) << ",\n";
  oss << "  \"status\": " << json_string(status) << ",\n";
  oss << "  \"started_at\": " << json_string(started_at) << ",\n";
  oss << "  \"ended_at\": ";
  if (ended_at.has_value()) {
    oss << json_string(*ended_at);
  } else {
    oss << "null";
  }
  oss << ",\n";
  oss << "  \"exit_code\": ";
  if (exit_code.has_value()) {
    oss << *exit_code;
  } else {
    oss << "null";
  }
  oss << "\n}\n";
  return oss.str();
}

std::string build_result_json(
    const std::string& run_id,
    const std::string& status,
    const int exit_code,
    const std::string& started_at,
    const std::string& ended_at,
    const long long duration_ms,
    const bool todo_update_applied,
    const std::string& summary_excerpt) {
  std::ostringstream oss;
  oss << "{\n";
  oss << "  \"run_id\": " << json_string(run_id) << ",\n";
  oss << "  \"status\": " << json_string(status) << ",\n";
  oss << "  \"exit_code\": " << exit_code << ",\n";
  oss << "  \"started_at\": " << json_string(started_at) << ",\n";
  oss << "  \"ended_at\": " << json_string(ended_at) << ",\n";
  oss << "  \"duration_ms\": " << duration_ms << ",\n";
  oss << "  \"todo_update_applied\": " << (todo_update_applied ? "true" : "false") << ",\n";
  oss << "  \"summary_excerpt\": " << json_string(summary_excerpt) << "\n";
  oss << "}\n";
  return oss.str();
}

} // namespace

int cmd_start(const std::optional<std::string>& maybe_project_name) {
  try {
    const std::string project_name = resolve_project_name(maybe_project_name);
    const fs::path projects_file = projects_file_path();
    const SelectedProjectPath selected_path = resolve_project_path(projects_file, project_name);
    const fs::path todo_file = project_dir_path(project_name) / "TODO.md";
    if (!fs::exists(todo_file)) {
      std::cerr << "ap start failed: TODO.md not found\n";
      return 1;
    }

    const std::optional<TodoTaskSelection> maybe_task = select_first_todo_task(todo_file);
    if (!maybe_task.has_value()) {
      std::cerr << "ap start failed: no runnable task found in TODO.md\n";
      return 1;
    }

    const std::string agent_name = resolve_agent_name();

    const TodoTaskSelection task = *maybe_task;
    const std::string run_id = current_run_id(task.source_line);
    const fs::path runtime_dir = project_dir_path(project_name) / "runtime";
    const fs::path runs_dir = runtime_dir / "runs";
    const fs::path run_dir = runs_dir / run_id;
    fs::create_directories(run_dir);

    const fs::path meta_file = run_dir / "meta.json";
    const fs::path prompt_file = run_dir / "prompt.txt";
    const fs::path stdout_file = run_dir / "stdout.log";
    const fs::path stderr_file = run_dir / "stderr.log";
    const fs::path result_file = run_dir / "result.json";
    const fs::path last_run_file = runtime_dir / "last_run.json";

    const std::string started_at = current_timestamp_with_offset();
    const std::string prompt = build_prompt(project_name, selected_path, task);
    write_text_file(prompt_file, prompt);
    write_text_file(
        meta_file,
        build_meta_json(
            run_id,
            project_name,
            task,
            selected_path,
            agent_name,
            "running",
            started_at,
            std::nullopt,
            std::nullopt));

    const auto started_clock = std::chrono::steady_clock::now();
    const AgentLaunchResult launch_result =
        run_agent(agent_name, prompt, selected_path.path, stdout_file, stderr_file);
    const auto ended_clock = std::chrono::steady_clock::now();

    const std::string ended_at = current_timestamp_with_offset();
    const long long duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     ended_clock - started_clock)
                                     .count();
    const bool succeeded = launch_result.exit_code == 0;
    const bool todo_update_applied = succeeded && mark_todo_task_done(todo_file, task);
    const std::string summary_excerpt = read_last_non_empty_line(stdout_file);
    const std::string status = succeeded ? "succeeded" : "failed";

    const std::string meta_json = build_meta_json(
        run_id,
        project_name,
        task,
        selected_path,
        launch_result.agent_name,
        status,
        started_at,
        ended_at,
        launch_result.exit_code);
    write_text_file(meta_file, meta_json);

    const std::string result_json = build_result_json(
        run_id,
        status,
        launch_result.exit_code,
        started_at,
        ended_at,
        duration_ms,
        todo_update_applied,
        summary_excerpt);
    write_text_file(result_file, result_json);
    write_text_file(last_run_file, result_json);

    if (succeeded) {
      std::cout << "completed task: " << task.title << '\n';
      return 0;
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
