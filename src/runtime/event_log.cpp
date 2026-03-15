#include "autopilot/runtime/event_log.hpp"

#include "autopilot/runtime/json_utils.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

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

std::string current_event_id(const std::size_t sequence) {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_tm{};
#if defined(_WIN32)
  localtime_s(&local_tm, &now_time);
#else
  localtime_r(&now_time, &local_tm);
#endif

  std::ostringstream oss;
  oss << "evt-" << std::put_time(&local_tm, "%Y%m%d-%H%M%S") << "-" << std::setw(4)
      << std::setfill('0') << sequence;
  return oss.str();
}

std::size_t count_existing_events(const fs::path& file_path) {
  if (!fs::exists(file_path)) {
    return 0;
  }

  std::ifstream in(file_path);
  if (!in) {
    throw std::runtime_error("failed to append event log");
  }

  std::size_t count = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) {
      ++count;
    }
  }
  return count;
}

std::string build_payload_json(const std::vector<EventPayloadField>& fields) {
  std::ostringstream oss;
  oss << "{";
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) {
      oss << ", ";
    }
    oss << json_string(fields[i].key) << ": " << fields[i].json_value;
  }
  oss << "}";
  return oss.str();
}

} // namespace

EventLog::EventLog(fs::path file_path)
    : file_path_(std::move(file_path)), next_sequence_(count_existing_events(file_path_) + 1) {
  fs::create_directories(file_path_.parent_path());
}

void EventLog::append(const std::string& project, const EventRecord& event) {
  const char* fail_event_type = std::getenv("AUTOPILOT_TEST_FAIL_EVENT_TYPE");
  if (fail_event_type != nullptr && event.type == fail_event_type) {
    throw std::runtime_error("failed to append event log");
  }

  std::ofstream out(file_path_, std::ios::app);
  if (!out) {
    throw std::runtime_error("failed to append event log");
  }

  out << "{"
      << "\"id\": " << json_string(current_event_id(next_sequence_++)) << ", "
      << "\"timestamp\": " << json_string(current_timestamp_with_offset()) << ", "
      << "\"project\": " << json_string(project) << ", "
      << "\"task_id\": " << json_nullable_string(event.task_id) << ", "
      << "\"run_id\": " << json_nullable_string(event.run_id) << ", "
      << "\"type\": " << json_string(event.type) << ", "
      << "\"actor\": " << json_string(event.actor) << ", "
      << "\"payload\": " << build_payload_json(event.payload_fields) << "}\n";

  if (!out) {
    throw std::runtime_error("failed to append event log");
  }
}

void EventLog::append_stream_file(
    const std::string& project,
    const std::string& task_id,
    const std::string& run_id,
    const std::string& actor,
    const std::string& stream,
    const fs::path& log_file,
    const std::size_t chunk_size) {
  std::ifstream in(log_file, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to replay run " + stream + " events");
  }

  std::size_t sequence = 1;
  while (true) {
    std::string chunk(chunk_size, '\0');
    in.read(&chunk[0], static_cast<std::streamsize>(chunk_size));
    const std::streamsize read_size = in.gcount();
    if (read_size <= 0) {
      break;
    }
    chunk.resize(static_cast<std::size_t>(read_size));

    const bool truncated = in.peek() != std::char_traits<char>::eof();
    append(
        project,
        EventRecord{
            task_id,
            run_id,
            stream == "stdout" ? "run.stdout" : "run.stderr",
            actor,
            {
                EventPayloadField{"stream", json_string(stream)},
                EventPayloadField{"sequence", std::to_string(sequence++)},
                EventPayloadField{"text", json_string(chunk)},
                EventPayloadField{"truncated", truncated ? "true" : "false"},
            },
        });
  }
}
