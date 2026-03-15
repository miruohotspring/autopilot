#include "autopilot/runtime/alert_store.hpp"

#include "autopilot/runtime/json_utils.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

std::string build_alert_json(const AlertRecord& alert) {
  std::ostringstream oss;
  oss << "{\n";
  oss << "  \"id\": " << json_string(alert.id) << ",\n";
  oss << "  \"project\": " << json_string(alert.project) << ",\n";
  oss << "  \"task_id\": " << json_string(alert.task_id) << ",\n";
  oss << "  \"run_id\": " << json_string(alert.run_id) << ",\n";
  oss << "  \"severity\": " << json_string(alert.severity) << ",\n";
  oss << "  \"type\": " << json_string(alert.type) << ",\n";
  oss << "  \"message\": " << json_string(alert.message) << ",\n";
  oss << "  \"created_at\": " << json_string(alert.created_at) << ",\n";
  oss << "  \"status\": " << json_string(alert.status) << "\n";
  oss << "}\n";
  return oss.str();
}

std::string allocate_next_alert_id(const fs::path& alerts_dir) {
  int max_id = 0;
  if (fs::exists(alerts_dir)) {
    for (const auto& entry : fs::directory_iterator(alerts_dir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json") {
        continue;
      }
      const std::string stem = entry.path().stem().string();
      if (stem.rfind("alert-", 0) != 0) {
        continue;
      }
      try {
        max_id = std::max(max_id, std::stoi(stem.substr(6)));
      } catch (const std::exception&) {
      }
    }
  }

  std::ostringstream oss;
  oss << "alert-" << std::setw(4) << std::setfill('0') << (max_id + 1);
  return oss.str();
}

void write_text_file(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to create alert file");
  }
  out << content;
  if (!out) {
    throw std::runtime_error("failed to create alert file");
  }
}

} // namespace

AlertStore::AlertStore(fs::path alerts_dir) : alerts_dir_(std::move(alerts_dir)) {}

AlertRecord AlertStore::create(
    const std::string& project,
    const std::string& task_id,
    const std::string& run_id,
    const AlertDraft& draft,
    const std::string& created_at) {
  fs::create_directories(alerts_dir_);

  AlertRecord alert{
      allocate_next_alert_id(alerts_dir_),
      project,
      task_id,
      run_id,
      draft.severity,
      draft.type,
      draft.message,
      created_at,
      "open",
  };
  write_text_file(alerts_dir_ / (alert.id + ".json"), build_alert_json(alert));
  return alert;
}
