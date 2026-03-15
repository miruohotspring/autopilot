#pragma once

#include "autopilot/runtime/run_result_classifier.hpp"

#include <filesystem>
#include <string>

struct AlertRecord {
  std::string id;
  std::string project;
  std::string task_id;
  std::string run_id;
  std::string severity;
  std::string type;
  std::string message;
  std::string created_at;
  std::string status;
};

class AlertStore {
 public:
  explicit AlertStore(std::filesystem::path alerts_dir);

  AlertRecord create(
      const std::string& project,
      const std::string& task_id,
      const std::string& run_id,
      const AlertDraft& draft,
      const std::string& created_at);

 private:
  std::filesystem::path alerts_dir_;
};
