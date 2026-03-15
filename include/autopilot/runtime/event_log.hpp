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
  void append_stream_file(
      const std::string& project,
      const std::string& task_id,
      const std::string& run_id,
      const std::string& actor,
      const std::string& stream,
      const std::filesystem::path& log_file,
      std::size_t chunk_size = 4096);

 private:
  std::filesystem::path file_path_;
  std::size_t next_sequence_;
};
