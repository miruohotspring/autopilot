#include "autopilot/runtime/lock_manager.hpp"

#include "autopilot/runtime/json_utils.hpp"

#include <cerrno>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr int kLockAcquireAttempts = 8;
constexpr int kUnreadableLockRecoveryAttempt = 3;
constexpr auto kStaleLockMargin = std::chrono::minutes(5);

std::string current_timestamp_with_offset() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_tm{};
  localtime_r(&now_time, &local_tm);
  std::ostringstream oss;
  oss << std::put_time(&local_tm, "%Y-%m-%dT%H:%M:%S%z");
  std::string timestamp = oss.str();
  if (timestamp.size() >= 5) {
    timestamp.insert(timestamp.size() - 2, ":");
  }
  return timestamp;
}

std::string get_hostname() {
  char buf[256] = {};
  if (::gethostname(buf, sizeof(buf) - 1) != 0) {
    return "unknown";
  }
  return std::string(buf);
}

std::string build_lock_json(
    int pid,
    const std::string& run_id,
    const std::optional<std::string>& task_id,
    const std::string& started_at,
    const std::string& hostname) {
  std::ostringstream oss;
  oss << "{\n";
  oss << "  \"pid\": " << pid << ",\n";
  oss << "  \"run_id\": " << json_string(run_id) << ",\n";
  oss << "  \"task_id\": " << json_nullable_string(task_id) << ",\n";
  oss << "  \"started_at\": " << json_string(started_at) << ",\n";
  oss << "  \"hostname\": " << json_string(hostname) << "\n";
  oss << "}\n";
  return oss.str();
}

bool write_all(const int fd, const std::string& content) {
  std::size_t total_written = 0;
  while (total_written < content.size()) {
    const ssize_t written =
        ::write(fd, content.data() + total_written, content.size() - total_written);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    total_written += static_cast<std::size_t>(written);
  }
  return true;
}

bool try_create_lock_file(const fs::path& path, const std::string& content) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    if (errno == EEXIST) {
      return false;
    }
    throw std::runtime_error("failed to create lock file: " + path.string());
  }

  bool success = write_all(fd, content);
  if (::close(fd) != 0) {
    success = false;
  }
  if (!success) {
    fs::remove(path);
    throw std::runtime_error("failed to write lock file: " + path.string());
  }
  return true;
}

void write_lock_file_atomically(const fs::path& path, const std::string& content) {
  const fs::path temp_path = path.string() + ".tmp";
  {
    std::ofstream out(temp_path, std::ios::trunc);
    if (!out) {
      throw std::runtime_error("failed to write lock file: " + temp_path.string());
    }
    out << content;
    if (!out) {
      throw std::runtime_error("failed to write lock file: " + temp_path.string());
    }
  }
  fs::rename(temp_path, path);
}

bool lock_info_matches(const LockInfo& lhs, const LockInfo& rhs) {
  return lhs.pid == rhs.pid && lhs.run_id == rhs.run_id && lhs.task_id == rhs.task_id;
}

bool remove_lock_if_owned(const fs::path& path, const LockInfo& expected) {
  const std::optional<LockInfo> current = LockManager::read_lock_file(path);
  if (!current.has_value()) {
    return !fs::exists(path);
  }
  if (!lock_info_matches(*current, expected)) {
    return false;
  }
  return fs::remove(path);
}

bool remove_unreadable_lock_file(const fs::path& path) {
  if (!fs::exists(path)) {
    return false;
  }
  return fs::remove(path);
}

bool rewrite_lock_if_owned(const fs::path& path, const LockInfo& expected, const LockInfo& desired) {
  const std::optional<LockInfo> current = LockManager::read_lock_file(path);
  if (!current.has_value() || !lock_info_matches(*current, expected)) {
    return false;
  }
  write_lock_file_atomically(
      path,
      build_lock_json(
          desired.pid,
          desired.run_id,
          desired.task_id,
          desired.started_at,
          desired.hostname));
  return true;
}

bool lock_age_exceeds(const fs::path& path, int timeout_seconds) {
  if (timeout_seconds <= 0) {
    return false;
  }

  std::error_code ec;
  const fs::file_time_type last_write_time = fs::last_write_time(path, ec);
  if (ec) {
    return false;
  }

  const auto now = std::chrono::system_clock::now();
  const auto translated_last_write = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      last_write_time - fs::file_time_type::clock::now() + now);
  return now - translated_last_write >
         std::chrono::seconds(timeout_seconds) + kStaleLockMargin;
}

} // namespace

LockManager::LockManager(fs::path lock_dir) : lock_dir_(std::move(lock_dir)) {
  fs::create_directories(lock_dir_);
}

fs::path LockManager::project_lock_path() const {
  return lock_dir_ / "project.lock";
}

fs::path LockManager::task_lock_path(const std::string& task_id) const {
  return lock_dir_ / ("task-" + task_id + ".lock");
}

std::optional<LockInfo> LockManager::read_lock_file(const fs::path& path) {
  if (!fs::exists(path)) {
    return std::nullopt;
  }

  std::ifstream in(path);
  if (!in) {
    return std::nullopt;
  }

  std::ostringstream oss;
  oss << in.rdbuf();
  const std::string json = oss.str();

  try {
    LockInfo info;
    info.pid = static_cast<int>(json_read_optional_integer(json, "pid").value_or(0));
    info.run_id = json_read_optional_string(json, "run_id").value_or("");
    info.task_id = json_read_optional_string(json, "task_id");
    info.started_at = json_read_optional_string(json, "started_at").value_or("");
    info.hostname = json_read_optional_string(json, "hostname").value_or("");
    return info;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

bool LockManager::is_stale_lock(const fs::path& path, const LockInfo& info, int timeout_seconds) {
  if (info.pid <= 0) {
    return true;
  }
  if (lock_age_exceeds(path, timeout_seconds)) {
    return true;
  }
  // Process is stale if kill(pid, 0) fails with ESRCH (no such process)
  if (::kill(info.pid, 0) == 0) {
    return false; // process exists
  }
  return errno == ESRCH; // EPERM means exists but no permission, so not stale
}

bool LockManager::acquire_project_lock(
    const std::string& run_id,
    int timeout_seconds,
    bool& was_stale,
    LockInfo& stale_info) {
  (void)timeout_seconds; // used only for stale margin (future enhancement)
  was_stale = false;

  const fs::path lock_path = project_lock_path();
  const LockInfo desired_info{
      static_cast<int>(::getpid()),
      run_id,
      std::nullopt,
      current_timestamp_with_offset(),
      get_hostname(),
  };
  const std::string content = build_lock_json(
      desired_info.pid,
      desired_info.run_id,
      desired_info.task_id,
      desired_info.started_at,
      desired_info.hostname);

  for (int attempt = 0; attempt < kLockAcquireAttempts; ++attempt) {
    const std::optional<LockInfo> existing = read_lock_file(lock_path);
    if (existing.has_value()) {
      if (is_stale_lock(lock_path, *existing, timeout_seconds)) {
        was_stale = true;
        stale_info = *existing;
        fs::remove(lock_path);
      } else {
        stale_info = *existing;
        return false;
      }
    } else if (fs::exists(lock_path)) {
      if (attempt >= kUnreadableLockRecoveryAttempt && remove_unreadable_lock_file(lock_path)) {
        was_stale = true;
        stale_info = LockInfo{0, "", std::nullopt, "", ""};
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    if (try_create_lock_file(lock_path, content)) {
      project_lock_acquired_ = true;
      project_lock_info_ = desired_info;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  stale_info = LockInfo{0, "", std::nullopt, "", ""};
  return false;
}

bool LockManager::acquire_task_lock(
    const std::string& task_id,
    const std::string& run_id,
    int timeout_seconds) {
  (void)timeout_seconds;

  const fs::path lock_path = task_lock_path(task_id);
  const LockInfo desired_info{
      static_cast<int>(::getpid()),
      run_id,
      task_id,
      current_timestamp_with_offset(),
      get_hostname(),
  };
  const std::string content = build_lock_json(
      desired_info.pid,
      desired_info.run_id,
      desired_info.task_id,
      desired_info.started_at,
      desired_info.hostname);

  for (int attempt = 0; attempt < kLockAcquireAttempts; ++attempt) {
    const std::optional<LockInfo> existing = read_lock_file(lock_path);
    if (existing.has_value()) {
      if (is_stale_lock(lock_path, *existing, timeout_seconds)) {
        fs::remove(lock_path);
      } else {
        return false;
      }
    } else if (fs::exists(lock_path)) {
      if (attempt >= kUnreadableLockRecoveryAttempt && remove_unreadable_lock_file(lock_path)) {
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    if (try_create_lock_file(lock_path, content)) {
      task_lock_acquired_ = true;
      locked_task_id_ = task_id;
      task_lock_info_ = desired_info;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return false;
}

void LockManager::transfer_project_lock_pid(int pid) {
  if (!project_lock_acquired_ || !project_lock_info_.has_value() || pid <= 0) {
    throw std::runtime_error("project lock is not transferable");
  }

  LockInfo desired = *project_lock_info_;
  desired.pid = pid;
  if (!rewrite_lock_if_owned(project_lock_path(), *project_lock_info_, desired)) {
    throw std::runtime_error("project lock ownership changed unexpectedly");
  }
  project_lock_info_ = desired;
}

void LockManager::transfer_task_lock_pid(const std::string& task_id, int pid) {
  if (!task_lock_acquired_ || locked_task_id_ != task_id || !task_lock_info_.has_value() || pid <= 0) {
    throw std::runtime_error("task lock is not transferable");
  }

  LockInfo desired = *task_lock_info_;
  desired.pid = pid;
  if (!rewrite_lock_if_owned(task_lock_path(task_id), *task_lock_info_, desired)) {
    throw std::runtime_error("task lock ownership changed unexpectedly");
  }
  task_lock_info_ = desired;
}

void LockManager::release_project_lock() {
  if (!project_lock_acquired_ || !project_lock_info_.has_value()) {
    return;
  }
  remove_lock_if_owned(project_lock_path(), *project_lock_info_);
  project_lock_acquired_ = false;
  project_lock_info_ = std::nullopt;
}

void LockManager::release_task_lock(const std::string& task_id) {
  if (!task_lock_acquired_ || locked_task_id_ != task_id || !task_lock_info_.has_value()) {
    return;
  }
  remove_lock_if_owned(task_lock_path(task_id), *task_lock_info_);
  task_lock_acquired_ = false;
  locked_task_id_.clear();
  task_lock_info_ = std::nullopt;
}

bool LockManager::has_project_lock() const {
  return project_lock_acquired_;
}

bool LockManager::has_task_lock() const {
  return task_lock_acquired_;
}
