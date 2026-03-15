#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct EventPayloadField {
  std::string key;
  std::string json_value;
};

struct EventRecord {
  std::optional<std::string> task_id;
  std::optional<std::string> run_id;
  std::string type;
  std::string actor;
  std::vector<EventPayloadField> payload_fields;
};

class EventLog {
 public:
  explicit EventLog(std::filesystem::path file_path);

  void append(const std::string& project, const EventRecord& event);

 private:
  std::filesystem::path file_path_;
  std::size_t next_sequence_;
};
