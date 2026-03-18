#pragma once

#include <filesystem>
#include <optional>
#include <string>

struct LockInfo {
  int pid;
  std::string run_id;
  std::optional<std::string> task_id;
  std::string started_at;
  std::string hostname;
};

class LockManager {
 public:
  explicit LockManager(std::filesystem::path lock_dir);

  // Returns true if lock was acquired.
  // Returns false if a live process holds the lock (stale_info set, was_stale=false).
  // Removes stale lock and acquires it (was_stale=true, stale_info set).
  bool acquire_project_lock(
      const std::string& run_id,
      int timeout_seconds,
      bool& was_stale,
      LockInfo& stale_info);

  // Returns true if task lock was acquired.
  bool acquire_task_lock(
      const std::string& task_id,
      const std::string& run_id,
      int timeout_seconds);

  void transfer_project_lock_pid(int pid);
  void transfer_task_lock_pid(const std::string& task_id, int pid);

  void release_project_lock();
  void release_task_lock(const std::string& task_id);

  bool has_project_lock() const;
  bool has_task_lock() const;

  std::filesystem::path project_lock_path() const;
  std::filesystem::path task_lock_path(const std::string& task_id) const;

  static std::optional<LockInfo> read_lock_file(const std::filesystem::path& path);
  static bool is_stale_lock(
      const std::filesystem::path& path,
      const LockInfo& info,
      int timeout_seconds);

private:
  std::filesystem::path lock_dir_;
  bool project_lock_acquired_ = false;
  bool task_lock_acquired_ = false;
  std::string locked_task_id_;
  std::optional<LockInfo> project_lock_info_;
  std::optional<LockInfo> task_lock_info_;
};
