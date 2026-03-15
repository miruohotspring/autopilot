#include "autopilot/runtime/task_state_store.hpp"

#include "autopilot/runtime/json_utils.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string read_text_file(const fs::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to open file: " + path.string());
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

TaskState parse_task_state_file(const fs::path& path) {
  try {
    const std::string json = read_text_file(path);
    TaskState task;
    task.id = json_read_required_string(json, "id");
    task.title = json_read_required_string(json, "title");
    task.description = json_read_optional_string(json, "description");
    task.status = json_read_required_string(json, "status");
    task.priority = static_cast<int>(json_read_optional_integer(json, "priority").value_or(100));
    task.depends_on = json_read_optional_string_array(json, "depends_on").value_or(
        std::vector<std::string>{});
    task.approval_required = json_read_optional_bool(json, "approval_required").value_or(false);
    task.related_paths =
        json_read_optional_string_array(json, "related_paths").value_or(std::vector<std::string>{});
    task.generated_by = json_read_optional_string(json, "generated_by").value_or("human.todo");
    task.source_file = json_read_required_string(json, "source_file");
    task.source_line = static_cast<std::size_t>(json_read_required_integer(json, "source_line"));
    task.source_text = json_read_required_string(json, "source_text");
    task.present_in_todo = json_read_required_bool(json, "present_in_todo");
    task.attempt_count = static_cast<int>(json_read_required_integer(json, "attempt_count"));
    task.latest_run_id = json_read_optional_string(json, "latest_run_id");
    task.last_error = json_read_optional_string(json, "last_error");
    task.blocker_reason = json_read_optional_string(json, "blocker_reason");
    task.blocker_category = json_read_optional_string(json, "blocker_category");
    task.created_at = json_read_required_string(json, "created_at");
    task.updated_at = json_read_required_string(json, "updated_at");
    return task;
  } catch (const std::exception&) {
    throw std::runtime_error("failed to read task state: " + path.string());
  }
}

std::string build_task_state_json(const TaskState& task) {
  std::ostringstream oss;
  oss << "{\n";
  oss << "  \"id\": " << json_string(task.id) << ",\n";
  oss << "  \"title\": " << json_string(task.title) << ",\n";
  oss << "  \"description\": " << json_nullable_string(task.description) << ",\n";
  oss << "  \"status\": " << json_string(task.status) << ",\n";
  oss << "  \"priority\": " << task.priority << ",\n";
  oss << "  \"depends_on\": " << json_string_array(task.depends_on) << ",\n";
  oss << "  \"approval_required\": " << (task.approval_required ? "true" : "false") << ",\n";
  oss << "  \"related_paths\": " << json_string_array(task.related_paths) << ",\n";
  oss << "  \"generated_by\": " << json_string(task.generated_by) << ",\n";
  oss << "  \"source_file\": " << json_string(task.source_file) << ",\n";
  oss << "  \"source_line\": " << task.source_line << ",\n";
  oss << "  \"source_text\": " << json_string(task.source_text) << ",\n";
  oss << "  \"present_in_todo\": " << (task.present_in_todo ? "true" : "false") << ",\n";
  oss << "  \"attempt_count\": " << task.attempt_count << ",\n";
  oss << "  \"latest_run_id\": " << json_nullable_string(task.latest_run_id) << ",\n";
  oss << "  \"last_error\": " << json_nullable_string(task.last_error) << ",\n";
  oss << "  \"blocker_reason\": " << json_nullable_string(task.blocker_reason) << ",\n";
  oss << "  \"blocker_category\": " << json_nullable_string(task.blocker_category) << ",\n";
  oss << "  \"created_at\": " << json_string(task.created_at) << ",\n";
  oss << "  \"updated_at\": " << json_string(task.updated_at) << "\n";
  oss << "}\n";
  return oss.str();
}

std::string build_project_state_json(const ProjectState& project) {
  std::ostringstream oss;
  oss << "{\n";
  oss << "  \"project\": " << json_string(project.project) << ",\n";
  oss << "  \"status\": " << json_string(project.status) << ",\n";
  oss << "  \"active_task_id\": " << json_nullable_string(project.active_task_id) << ",\n";
  oss << "  \"last_run_id\": " << json_nullable_string(project.last_run_id) << ",\n";
  oss << "  \"last_run_at\": " << json_nullable_string(project.last_run_at) << ",\n";
  oss << "  \"task_counts\": {\n";
  oss << "    \"todo\": " << project.task_counts.todo << ",\n";
  oss << "    \"in_progress\": " << project.task_counts.in_progress << ",\n";
  oss << "    \"review_pending\": " << project.task_counts.review_pending << ",\n";
  oss << "    \"blocked\": " << project.task_counts.blocked << ",\n";
  oss << "    \"done\": " << project.task_counts.done << ",\n";
  oss << "    \"failed\": " << project.task_counts.failed << ",\n";
  oss << "    \"cancelled\": " << project.task_counts.cancelled << "\n";
  oss << "  },\n";
  oss << "  \"updated_at\": " << json_string(project.updated_at) << "\n";
  oss << "}\n";
  return oss.str();
}

} // namespace

std::vector<TaskState> load_task_states(const fs::path& tasks_dir) {
  std::vector<TaskState> tasks;
  if (!fs::exists(tasks_dir)) {
    return tasks;
  }

  for (const auto& entry : fs::directory_iterator(tasks_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") {
      continue;
    }
    tasks.push_back(parse_task_state_file(entry.path()));
  }

  std::sort(tasks.begin(), tasks.end(), [](const TaskState& lhs, const TaskState& rhs) {
    return lhs.id < rhs.id;
  });
  return tasks;
}

std::optional<ProjectState> load_project_state(const fs::path& project_file) {
  if (!fs::exists(project_file)) {
    return std::nullopt;
  }

  const std::string json = read_text_file(project_file);
  ProjectState project;
  project.project = json_read_required_string(json, "project");
  project.status = json_read_required_string(json, "status");
  project.active_task_id = json_read_optional_string(json, "active_task_id");
  project.last_run_id = json_read_optional_string(json, "last_run_id");
  project.last_run_at = json_read_optional_string(json, "last_run_at");
  project.task_counts.todo = static_cast<int>(json_read_required_integer(json, "todo"));
  project.task_counts.in_progress =
      static_cast<int>(json_read_required_integer(json, "in_progress"));
  project.task_counts.review_pending =
      static_cast<int>(json_read_optional_integer(json, "review_pending").value_or(0));
  project.task_counts.blocked =
      static_cast<int>(json_read_optional_integer(json, "blocked").value_or(0));
  project.task_counts.done = static_cast<int>(json_read_required_integer(json, "done"));
  project.task_counts.failed = static_cast<int>(json_read_required_integer(json, "failed"));
  project.task_counts.cancelled =
      static_cast<int>(json_read_optional_integer(json, "cancelled").value_or(0));
  project.updated_at = json_read_required_string(json, "updated_at");
  return project;
}

void save_task_state(const fs::path& tasks_dir, const TaskState& task) {
  fs::create_directories(tasks_dir);
  write_text_file(tasks_dir / (task.id + ".json"), build_task_state_json(task));
}

void save_task_states(const fs::path& tasks_dir, const std::vector<TaskState>& tasks) {
  fs::create_directories(tasks_dir);
  for (const TaskState& task : tasks) {
    save_task_state(tasks_dir, task);
  }
}

void save_project_state(const fs::path& project_file, const ProjectState& project) {
  fs::create_directories(project_file.parent_path());
  write_text_file(project_file, build_project_state_json(project));
}

std::string allocate_next_task_id(
    const std::vector<TaskState>& tasks, const std::string& project_slug) {
  int max_id = 0;
  for (const TaskState& task : tasks) {
    if (task.id.rfind(project_slug + "-", 0) != 0) {
      continue;
    }
    const std::string numeric = task.id.substr(project_slug.size() + 1);
    if (numeric.size() != 4) {
      continue;
    }
    try {
      max_id = std::max(max_id, std::stoi(numeric));
    } catch (const std::exception&) {
    }
  }

  std::ostringstream oss;
  oss << project_slug << '-' << std::setw(4) << std::setfill('0') << (max_id + 1);
  return oss.str();
}

ProjectTaskCounts compute_project_task_counts(const std::vector<TaskState>& tasks) {
  ProjectTaskCounts counts;
  for (const TaskState& task : tasks) {
    if (!task.present_in_todo) {
      continue;
    }
    if (task.status == "todo") {
      ++counts.todo;
    } else if (task.status == "in_progress") {
      ++counts.in_progress;
    } else if (task.status == "review_pending") {
      ++counts.review_pending;
    } else if (task.status == "blocked") {
      ++counts.blocked;
    } else if (task.status == "done") {
      ++counts.done;
    } else if (task.status == "failed") {
      ++counts.failed;
    } else if (task.status == "cancelled") {
      ++counts.cancelled;
    }
  }
  return counts;
}
